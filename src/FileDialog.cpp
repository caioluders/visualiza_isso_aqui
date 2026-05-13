#include "FileDialog.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <sstream>

namespace {

std::string shellQuote(const std::string& value) {
	std::string out = "'";
	for (char c : value) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	out += "'";
	return out;
}

std::string runDialogCommand(const std::string& command) {
	std::array<char, 512> buffer{};
	std::string result;
	FILE* pipe = popen(command.c_str(), "r");
	if (!pipe) return result;
	while (fgets(buffer.data(), (int)buffer.size(), pipe)) {
		result += buffer.data();
	}
	pclose(pipe);
	while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
	return result;
}

bool hasCommand(const char* command) {
	std::string check = std::string("command -v ") + command + " >/dev/null 2>&1";
	return std::system(check.c_str()) == 0;
}

} // namespace

std::string openFileDialog(const char* title, const char* directory, const std::vector<const char*>& exts) {
	std::string dialogTitle = title ? title : "Open file";
	std::string startDir = directory ? directory : "";
	if (hasCommand("zenity")) {
		std::ostringstream cmd;
		cmd << "zenity --file-selection --title=" << shellQuote(dialogTitle);
		if (!startDir.empty()) cmd << " --filename=" << shellQuote(startDir + "/");
		if (!exts.empty()) {
			cmd << " --file-filter=" << shellQuote("Allowed files | *." + std::string(exts.front()));
		}
		cmd << " 2>/dev/null";
		return runDialogCommand(cmd.str());
	}
	if (hasCommand("kdialog")) {
		std::ostringstream cmd;
		cmd << "kdialog --getopenfilename ";
		cmd << (startDir.empty() ? "." : shellQuote(startDir));
		if (!exts.empty()) {
			cmd << " " << shellQuote("*." + std::string(exts.front()));
		}
		cmd << " --title " << shellQuote(dialogTitle) << " 2>/dev/null";
		return runDialogCommand(cmd.str());
	}
	return std::string();
}
