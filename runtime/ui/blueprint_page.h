// The Blueprint tab (ui/menu.cpp): the host's file dialog (folder_pick.h file_pick) picks a .cai blueprint file, and the
// page shows what it holds, lets the player choose one of the game's 30 Autobuild favourite slots, and writes the blob
// into the save data (blueprint_import.h). The first phase of the import only kept the path; this page decodes the file
// with blueprint_cai.h and hands it to the game's own Autobuild data, so the native preview, rotation and Zonai energy
// costs work unchanged.
#pragma once

void tab_blueprint(float scale);
