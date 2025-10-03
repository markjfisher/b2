#include <shared/system.h>
#include "native_ui.h"
#include "native_ui_gtk.h"
#include <glib-2.0/glib.h>
#include <gtk/gtk.h>
#include "misc.h"
#include "Messages.h"
#include <SDL.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "load_save.h"

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// I couldn't get gtk_clipboard_set_image to work, and it's a pain
// having to faff about with the GdxPixbuf stuff anyway. So this
// shells out to xclip.
//
// As a bonus, xclip does a sort of auto-daemonize kind of thing so
// the clipped image data can live on after b2 quits.

static void RunXClip(const std::string &temp_file_path,
                     Messages *messages) {

    // Ugh
    char *argv[] = {
        (char *)"xclip",
        (char *)"-selection",
        (char *)"clipboard",
        (char *)"-target",
        (char *)"image/png",
        (char *)"-in",
        (char *)temp_file_path.c_str(),
        nullptr,
    };
    pid_t xclip_pid;
    int rc = posix_spawnp(&xclip_pid, "xclip", nullptr, nullptr, argv, environ);
    if (rc != 0) {
        messages->e.f("Failed to run xclip: %s\n", strerror(rc));
        return;
    }

    int status;
    if (waitpid(xclip_pid, &status, 0) != xclip_pid) {
        messages->e.f("xclip failed: %s\n", strerror(errno));
        return;
    }

    if (!WIFEXITED(status)) {
        messages->e.f("xclip didn't exit\n");
        return;
    }

    if (WEXITSTATUS(status) != 0) {
        messages->e.f("xclip failed with exit code %d\n", WEXITSTATUS(status));
        return;
    }
}

void SetClipboardImage(SDL_Surface *surface, Messages *messages) {
    char temp_file_path[] = "/tmp/b2_png_XXXXXX";
    int fd = mkstemp(temp_file_path);
    if (fd == -1) {
        messages->e.f("Failed to open temp file: %s\n", strerror(errno));
        return;
    }

    close(fd);
    fd = -1;

    if (SaveSDLSurface(surface, temp_file_path, messages)) {
        RunXClip(temp_file_path, messages);
    }

    unlink(temp_file_path);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void MessageBox(const std::string &title, const std::string &text) {
    // Get the main application window as parent
    GtkWindow *parent = nullptr;
    GList *toplevels = gtk_window_list_toplevels();
    if (toplevels) {
        for (GList *iter = toplevels; iter; iter = iter->next) {
            GtkWidget *window = GTK_WIDGET(iter->data);
            if (gtk_widget_get_visible(window) && GTK_IS_WINDOW(window)) {
                parent = GTK_WINDOW(window);
                break;
            }
        }
        g_list_free(toplevels);
    }
    
    // Use GTK4's GtkAlertDialog instead of deprecated GtkMessageDialog
    GtkAlertDialog *alert = gtk_alert_dialog_new(title.c_str());
    gtk_alert_dialog_set_detail(alert, text.c_str());
    gtk_alert_dialog_set_modal(alert, TRUE);
    
    // Show the alert dialog
    gtk_alert_dialog_show(alert, parent);
    
    // Clean up
    g_object_unref(alert);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static GtkWidget *CreateFileDialog(const char *title,
                                   GtkFileChooserAction action) {
    // Get the main application window as parent
    GtkWindow *parent = nullptr;
    GList *toplevels = gtk_window_list_toplevels();
    if (toplevels) {
        for (GList *iter = toplevels; iter; iter = iter->next) {
            GtkWidget *window = GTK_WIDGET(iter->data);
            if (gtk_widget_get_visible(window) && GTK_IS_WINDOW(window)) {
                parent = GTK_WINDOW(window);
                break;
            }
        }
        g_list_free(toplevels);
    }
    
    const char *accept_button_text = (action == GTK_FILE_CHOOSER_ACTION_SAVE) ? "_Save" : "_Open";
    
    // Note: gtk_file_chooser_dialog_new is deprecated in GTK4, but we keep it for legacy compatibility
    // The new async dialogs use GtkFileDialog instead
    GtkWidget *gdialog = gtk_file_chooser_dialog_new(title,
                                                     parent,
                                                     action,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     accept_button_text, GTK_RESPONSE_ACCEPT,
                                                     nullptr);
    return gdialog;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// Forward declaration for TraceUI callback
class TraceUI;
static void (*g_trace_save_callback)(const std::string& path) = nullptr;

// For std::function callbacks
static std::function<void(const std::string&)> g_std_function_callback;

// Dialog operation types
enum class DialogOperation {
    Save,
    Open,
    SelectFolder
};

static DialogOperation g_current_dialog_operation = DialogOperation::Save;

// Function to process GTK events (to be called from main SDL loop)
void ProcessGTKEvents() {
    // Process any pending GTK events without blocking
    while (g_main_context_pending(g_main_context_default())) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }
}


// Callback for GTK4 async file dialog
static void async_file_dialog_response_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)user_data; // Suppress unused parameter warning
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = nullptr;
    
    // Use the correct finish function based on the operation type
    GFile *file = nullptr;
    switch (g_current_dialog_operation) {
        case DialogOperation::Save:
            file = gtk_file_dialog_save_finish(dialog, res, &error);
            break;
        case DialogOperation::Open:
            file = gtk_file_dialog_open_finish(dialog, res, &error);
            break;
        case DialogOperation::SelectFolder:
            file = gtk_file_dialog_select_folder_finish(dialog, res, &error);
            break;
    }
    
    if (error) {
        g_error_free(error);
        if (g_trace_save_callback) {
            g_trace_save_callback(""); // Empty path indicates error/cancellation
        }
        if (g_std_function_callback) {
            g_std_function_callback(""); // Empty path indicates error/cancellation
        }
    } else if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            if (g_trace_save_callback) {
                g_trace_save_callback(std::string(path));
            }
            if (g_std_function_callback) {
                g_std_function_callback(std::string(path));
            }
            g_free(path);
        }
        g_object_unref(file);
    } else {
        if (g_trace_save_callback) {
            g_trace_save_callback(""); // Empty path indicates cancellation
        }
        if (g_std_function_callback) {
            g_std_function_callback(""); // Empty path indicates cancellation
        }
    }
    
    // Clean up the dialog now that the async operation is complete
    g_object_unref(dialog);
}

// New async function for TraceUI Save
void SaveFileDialogGTKAsync(const std::vector<OpenFileDialog::Filter> &filters,
                           const std::string &default_path,
                           void (*callback)(const std::string& path)) {
    (void)filters; // Suppress unused parameter warning
    (void)default_path; // Suppress unused parameter warning
    // Get the main application window as parent
    GtkWindow *parent = nullptr;
    GList *toplevels = gtk_window_list_toplevels();
    if (toplevels) {
        for (GList *iter = toplevels; iter; iter = iter->next) {
            GtkWidget *window = GTK_WIDGET(iter->data);
            if (gtk_widget_get_visible(window) && GTK_IS_WINDOW(window)) {
                parent = GTK_WINDOW(window);
                break;
            }
        }
        g_list_free(toplevels);
    }
    
    // Create GTK4 native file dialog
    GtkFileDialog *file_dialog = gtk_file_dialog_new();
    
    // Set dialog properties
    gtk_file_dialog_set_title(file_dialog, "Save File");
    
    // Set the callback and operation type for this operation
    g_trace_save_callback = callback;
    g_current_dialog_operation = DialogOperation::Save;
    
    // Start the async file dialog (non-blocking)
    gtk_file_dialog_save(file_dialog, parent, nullptr, async_file_dialog_response_callback, nullptr);
    
    // Don't unref the dialog - it needs to stay alive for the async operation
    // The dialog will be cleaned up in the callback
}

// Async function for Open File Dialog
void OpenFileDialogGTKAsync(const std::vector<OpenFileDialog::Filter> &filters,
                           const std::string &default_path,
                           std::function<void(const std::string&)> callback) {
    (void)filters; // Suppress unused parameter warning - filters not yet implemented for GTK4
    
    // Get the main application window as parent
    GtkWindow *parent = nullptr;
    GList *toplevels = gtk_window_list_toplevels();
    if (toplevels) {
        for (GList *iter = toplevels; iter; iter = iter->next) {
            GtkWidget *window = GTK_WIDGET(iter->data);
            if (gtk_widget_get_visible(window) && GTK_IS_WINDOW(window)) {
                parent = GTK_WINDOW(window);
                break;
            }
        }
        g_list_free(toplevels);
    }
    
    // Create GTK4 native file dialog
    GtkFileDialog *file_dialog = gtk_file_dialog_new();
    
    // Set dialog properties
    gtk_file_dialog_set_title(file_dialog, "Open File");
    
    // Set initial folder if provided
    if (!default_path.empty()) {
        GFile *initial_folder = g_file_new_for_path(default_path.c_str());
        gtk_file_dialog_set_initial_folder(file_dialog, initial_folder);
        g_object_unref(initial_folder);
    }
    
    // Store the callback for this operation
    g_std_function_callback = callback;
    g_current_dialog_operation = DialogOperation::Open;
    
    // Start the async file dialog (non-blocking)
    gtk_file_dialog_open(file_dialog, parent, nullptr, async_file_dialog_response_callback, nullptr);
    
    // Don't unref the dialog - it needs to stay alive for the async operation
    // The dialog will be cleaned up in the callback
}

// Async function for Select Folder Dialog
void SelectFolderDialogGTKAsync(const std::string &default_path,
                               void (*callback)(const std::string& path)) {
    
    // Get the main application window as parent
    GtkWindow *parent = nullptr;
    GList *toplevels = gtk_window_list_toplevels();
    if (toplevels) {
        for (GList *iter = toplevels; iter; iter = iter->next) {
            GtkWidget *window = GTK_WIDGET(iter->data);
            if (gtk_widget_get_visible(window) && GTK_IS_WINDOW(window)) {
                parent = GTK_WINDOW(window);
                break;
            }
        }
        g_list_free(toplevels);
    }
    
    // Create GTK4 native file dialog
    GtkFileDialog *file_dialog = gtk_file_dialog_new();
    
    // Set dialog properties
    gtk_file_dialog_set_title(file_dialog, "Select Folder");
    
    // Set initial folder if provided
    if (!default_path.empty()) {
        GFile *initial_folder = g_file_new_for_path(default_path.c_str());
        gtk_file_dialog_set_initial_folder(file_dialog, initial_folder);
        g_object_unref(initial_folder);
    }
    
    // Set the callback and operation type for this operation
    g_trace_save_callback = callback;
    g_current_dialog_operation = DialogOperation::SelectFolder;
    
    // Start the async folder selection dialog (non-blocking)
    gtk_file_dialog_select_folder(file_dialog, parent, nullptr, async_file_dialog_response_callback, nullptr);
    
    // Don't unref the dialog - it needs to stay alive for the async operation
    // The dialog will be cleaned up in the callback
}

static std::string RunFileDialog(GtkWidget *gdialog) {
    (void)gdialog; // Suppress unused parameter warning
    // Get the main application window as parent
    GtkWindow *parent = nullptr;
    GList *toplevels = gtk_window_list_toplevels();
    if (toplevels) {
        for (GList *iter = toplevels; iter; iter = iter->next) {
            GtkWidget *window = GTK_WIDGET(iter->data);
            if (gtk_widget_get_visible(window) && GTK_IS_WINDOW(window)) {
                parent = GTK_WINDOW(window);
                break;
            }
        }
        g_list_free(toplevels);
    }
    
    // Create GTK4 native file dialog
    GtkFileDialog *file_dialog = gtk_file_dialog_new();
    
    // Set dialog properties
    gtk_file_dialog_set_title(file_dialog, "Open File");
    
    // Clear the callback for open operations
    g_trace_save_callback = nullptr;
    
    // Start the async file dialog (non-blocking)
    gtk_file_dialog_open(file_dialog, parent, nullptr, async_file_dialog_response_callback, nullptr);
    
    // Process events to ensure dialog appears
    while (g_main_context_pending(g_main_context_default())) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }
    
    g_object_unref(file_dialog);
    
    // Return empty result - the dialog will handle the result asynchronously
    // This is a compromise: we avoid ANR but lose the synchronous return value
    // The real solution would be to restructure the calling code to handle async results
    return "";
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static void SetDefaultPath(GtkWidget *gdialog,
                           const std::string &default_path) {
    if (!default_path.empty()) {
        GFile *file = g_file_new_for_path(default_path.c_str());
        // Note: gtk_file_chooser_set_file is deprecated in GTK4, but we keep it for legacy compatibility
        gtk_file_chooser_set_file(GTK_FILE_CHOOSER(gdialog), file, nullptr);
        g_object_unref(file);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static void AddFilters(GtkWidget *gdialog,
                       const std::vector<OpenFileDialog::Filter> &filters) {
    for (const OpenFileDialog::Filter &filter : filters) {
        GtkFileFilter *gfilter = gtk_file_filter_new();

        std::string name = filter.title + " (";
        for (size_t i = 0; i < filter.extensions.size(); ++i) {
            if (i > 0) {
                name += "; ";
            }
            name += "*" + filter.extensions[i];
        }
        name += ")";

        gtk_file_filter_set_name(gfilter, name.c_str());

        for (const std::string &extension : filter.extensions) {
            gtk_file_filter_add_pattern(gfilter, ("*" + extension).c_str());
        }

        // Note: gtk_file_chooser_add_filter is deprecated in GTK4, but we keep it for legacy compatibility
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(gdialog), gfilter);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::string OpenFileDialogGTK(const std::vector<OpenFileDialog::Filter> &filters,
                              const std::string &default_path) {
    GtkWidget *gdialog = CreateFileDialog("Open File",
                                          GTK_FILE_CHOOSER_ACTION_OPEN);

    AddFilters(gdialog, filters);
    SetDefaultPath(gdialog, default_path);

    return RunFileDialog(gdialog);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::string SaveFileDialogGTK(const std::vector<OpenFileDialog::Filter> &filters,
                              const std::string &default_path) {
    GtkWidget *gdialog = CreateFileDialog("Save File",
                                          GTK_FILE_CHOOSER_ACTION_SAVE);

    AddFilters(gdialog, filters);
    SetDefaultPath(gdialog, default_path);
    // Note: gtk_file_chooser_set_do_overwrite_confirmation is not available in GTK4

    return RunFileDialog(gdialog);
}

// GTK-specific async implementation for SaveFileDialog
void SaveFileDialogGTKAsync(const std::vector<OpenFileDialog::Filter> &filters,
                           const std::string &default_path,
                           std::function<void(const std::string&)> callback) {
    (void)filters; // Suppress unused parameter warning - filters not yet implemented for GTK4
    
    // Get the main application window as parent
    GtkWindow *parent = nullptr;
    GList *toplevels = gtk_window_list_toplevels();
    if (toplevels) {
        for (GList *iter = toplevels; iter; iter = iter->next) {
            GtkWidget *window = GTK_WIDGET(iter->data);
            if (gtk_widget_get_visible(window) && GTK_IS_WINDOW(window)) {
                parent = GTK_WINDOW(window);
                break;
            }
        }
        g_list_free(toplevels);
    }
    
    // Create GTK4 native file dialog
    GtkFileDialog *file_dialog = gtk_file_dialog_new();
    
    // Set dialog properties
    gtk_file_dialog_set_title(file_dialog, "Save File");
    
    // Set initial folder if provided
    if (!default_path.empty()) {
        GFile *initial_folder = g_file_new_for_path(default_path.c_str());
        gtk_file_dialog_set_initial_folder(file_dialog, initial_folder);
        g_object_unref(initial_folder);
    }
    
    // Store the callback for this operation
    g_std_function_callback = callback;
    g_current_dialog_operation = DialogOperation::Save;
    
    // Start the async file dialog (non-blocking)
    gtk_file_dialog_save(file_dialog, parent, nullptr, async_file_dialog_response_callback, nullptr);

}


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::string SelectFolderDialogGTK(const std::string &default_path) {
    GtkWidget *gdialog = CreateFileDialog("Select Folder",
                                          GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);

    SetDefaultPath(gdialog, default_path);

    return RunFileDialog(gdialog);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
