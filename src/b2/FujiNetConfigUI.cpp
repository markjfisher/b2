#include <shared/system.h>
#include "FujiNetConfigUI.h"
#include "FujiNetConfig.h"
#include "dear_imgui.h"
#include "native_ui.h"

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static const char FUJINET_POPUP[] = "fujinet_popup";

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool DoFujiNetConfigUI(B2FujiNetConfig *config,
                       OpenFileDialog *device_ofd,
                       RecentPaths *recent_paths,
                       SDL_Window *window) {
    bool edited = false;

    // Interface type selection
    {
        const char *interface_items[] = {"Serial", "User Port"};
        int interface_current = (int)config->interface_type.value;
        if (ImGui::Combo("Interface", &interface_current, interface_items, 2)) {
            config->interface_type = (FujiNetInterfaceType)interface_current;
            edited = true;
        }
        
        if (config->interface_type == FujiNetInterfaceType_Serial) {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses BBC Micro serial port (ACIA 6850)");
            }
        } else {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses BBC Micro user port");
            }
        }
    }

    // Device mode selection
    {
        const char *mode_items[] = {"PTY/Virtual", "Hardware Serial"};
        int mode_current = (int)config->device_mode.value;
        if (ImGui::Combo("Device Mode", &mode_current, mode_items, 2)) {
            config->device_mode = (FujiNetDeviceMode)mode_current;
            edited = true;
        }
        
        if (config->device_mode == FujiNetDeviceMode_PTY) {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Virtual PTY for testing with bridge/socat (e.g., /tmp/fn-tty)");
            }
        } else {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Real hardware serial port (e.g., /dev/ttyUSB0)");
            }
        }
    }

    // Device path input
    if (ImGuiInputText(&config->device_path,
                       "Device Path",
                       config->device_path)) {
        edited = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("...##fujinet")) {
        ImGui::OpenPopup(FUJINET_POPUP);
    }
    
    if (ImGui::BeginPopup(FUJINET_POPUP)) {
        if (ImGui::MenuItem("File...")) {
            if (device_ofd->Open(window, &config->device_path)) {
                device_ofd->AddLastPathToRecentPaths(recent_paths);
                edited = true;
            }
        }
        
        if (ImGuiRecentMenu(&config->device_path, "Recent device", recent_paths)) {
            edited = true;
        }
        
        ImGui::Separator();
        
        // Quick paths for common devices
        if (ImGui::BeginMenu("Common Paths")) {
            if (ImGui::MenuItem("/tmp/fn-tty (Default PTY)")) {
                config->device_path = "/tmp/fn-tty";
                edited = true;
            }
            if (ImGui::MenuItem("/tmp/inject-tty (Test PTY)")) {
                config->device_path = "/tmp/inject-tty";
                edited = true;
            }
            if (ImGui::MenuItem("/dev/ttyUSB0 (Hardware)")) {
                config->device_path = "/dev/ttyUSB0";
                edited = true;
            }
            if (ImGui::MenuItem("/dev/ttyS0 (Hardware)")) {
                config->device_path = "/dev/ttyS0";
                edited = true;
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndPopup();
    }

    // Auto-connect checkbox
    if (ImGui::Checkbox("Auto-connect on startup", &config->auto_connect)) {
        edited = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically connect to FujiNet device when emulator starts");
    }

    // Debug logging checkbox
    if (ImGui::Checkbox("Enable debug logging##fujinet", &config->debug)) {
        edited = true;
    }
    
    // Help text
    ImGuiStyleColourPusher pusher;
    pusher.PushDefault(ImGuiCol_Text);
    ImGui::TextWrapped("Network adapter for BBC Micro. Use bridge tool or real FujiNet hardware.");

    return edited;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

