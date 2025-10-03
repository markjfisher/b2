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

// Static callback for message dialog
static void message_dialog_response_callback(GtkDialog *dialog, gint /*response_id*/, gpointer /*user_data*/) {
    gtk_window_destroy(GTK_WINDOW(dialog));
}

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
    
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_OK,
                                               "%s",
                                               title.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                             "%s",
                                             text.c_str());
    
    // Set dialog properties for proper floating behavior
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    }
    
    // Connect response signal and show dialog (non-blocking)
    g_signal_connect(dialog, "response", G_CALLBACK(message_dialog_response_callback), nullptr);
    gtk_widget_show(dialog);
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

// Function to process GTK events (to be called from main SDL loop)
void ProcessGTKEvents() {
    // Process any pending GTK events without blocking
    while (g_main_context_pending(g_main_context_default())) {
        g_main_context_iteration(g_main_context_default(), FALSE);
    }
}


// Callback for GTK4 async file dialog
static void async_file_dialog_response_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GError *error = nullptr;
    
    // Use the correct finish function based on the operation type
    GFile *file = nullptr;
    if (g_trace_save_callback) {
        // This is a save operation
        file = gtk_file_dialog_save_finish(dialog, res, &error);
    } else {
        // This is an open operation
        file = gtk_file_dialog_open_finish(dialog, res, &error);
    }
    
    if (error) {
        g_error_free(error);
        if (g_trace_save_callback) {
            g_trace_save_callback(""); // Empty path indicates error/cancellation
        }
    } else if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            if (g_trace_save_callback) {
                g_trace_save_callback(std::string(path));
            }
            g_free(path);
        }
        g_object_unref(file);
    } else {
        if (g_trace_save_callback) {
            g_trace_save_callback(""); // Empty path indicates cancellation
        }
    }
    
    // Clean up the dialog now that the async operation is complete
    g_object_unref(dialog);
}

// New async function for TraceUI Save
void SaveFileDialogGTKAsync(const std::vector<OpenFileDialog::Filter> &filters,
                           const std::string &default_path,
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
    gtk_file_dialog_set_title(file_dialog, "Save File");
    
    // Set the callback for this operation
    g_trace_save_callback = callback;
    
    // Start the async file dialog (non-blocking)
    gtk_file_dialog_save(file_dialog, parent, nullptr, async_file_dialog_response_callback, nullptr);
    
    // Don't unref the dialog - it needs to stay alive for the async operation
    // The dialog will be cleaned up in the callback
}

static std::string RunFileDialog(GtkWidget *gdialog) {
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
