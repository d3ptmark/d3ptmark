#ifndef __PT_UI_H__
#define __PT_UI_H__

void R_PT_InitUI();

#ifndef IMGUI_DISABLE
void R_PT_DrawUI();
void R_PT_OpenUI( const char *tab );
#endif

#endif
