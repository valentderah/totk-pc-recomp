// Right-to-left lines in the in-game menu (rtl_imgui.cpp).
#pragma once

// Makes Dear ImGui draw every line that holds Arabic-script text in display order (rtl_text.c rtl_visual), and
// right-aligns the lines of a wrapped text block that holds any. Call once after ImGui::CreateContext.
void rtl_imgui_install(void);
