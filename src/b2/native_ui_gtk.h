#ifndef HEADER_95200508464B4360923B8FC98607A039 // -*- mode:c++ -*-
#define HEADER_95200508464B4360923B8FC98607A039

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include <vector>
#include <string>

// Forward declarations
class OpenFileDialog;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void MessageBox(const std::string &title, const std::string &text);

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::string OpenFileDialogGTK(const std::vector<OpenFileDialog::Filter> &filters,
                              const std::string &default_path);

std::string SaveFileDialogGTK(const std::vector<OpenFileDialog::Filter> &filters,
                              const std::string &default_path);

void SaveFileDialogGTKAsync(const std::vector<OpenFileDialog::Filter> &filters,
                           const std::string &default_path,
                           void (*callback)(const std::string& path));

// Function to process GTK events (to be called from main SDL loop)
void ProcessGTKEvents();

std::string SelectFolderDialogGTK(const std::string &default_path);

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif
