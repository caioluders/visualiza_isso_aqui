#pragma once

#include <string>
#include <vector>

// Simple cross-platform file open dialog (currently implemented for macOS)
// Returns empty string if cancelled.
std::string openFileDialog(const char* title,
							 const char* directory,
							 const std::vector<const char*>& allowedExtensions);


