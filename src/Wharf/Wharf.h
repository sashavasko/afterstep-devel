#define IN_MODULE
#define MODULE_X_INTERFACE
#include "../../include/asapp.h"
#include "../../include/afterstep.h"
#include "../../include/parse.h"
#include "../../include/screen.h"
#include "../../include/module.h"

#ifdef MAX_BUTTONS
#undef MAX_BUTTONS
#endif

#define MAX_BUTTONS 100

#define MAX_OVERLAY 3

#define BUTTON_ARRAY_LN 102
#define FOLDER_ARRAY_LN 10

#define ICON_WIN_WIDTH	48
#define ICON_WIN_HEIGHT 48

#define ISMAPPED 0
#define NOTMAPPED 1

#ifndef get_flags
#define get_flags(var, val) ((var) & (val))
#define set_flags(var, val) ((var) |= (val))
#define clear_flags(var, val) ((var) &= ~(val))
#endif

#define DOUBLECLICKTIME 1

enum /* button_info flags */
{
  WB_Shaped		= (1 <<  0), 	/* is the button shaped? */
  WB_MaxSize		= (1 <<  1), 	/* was MaxSwallow specified? */
  WB_UserSetSize	= (1 <<  2), 	/* was Size specified? */
  WB_Transient		= (1 <<  3), 	/* was Transient specified? */
  WB_Pressed		= (1 <<  4),	/* is the button pressed? */
  WB_Module		= (1 <<  5)	/* was SwallowModule specified? */
};

typedef struct button_info
{
  button_info* next;
  folder_info* parent;			/* folder containing this button */
  folder_info* folder;			/* folder this button contains */
  char *action;
  char *title;
  int num_icons;
  ASImage *icons[MAX_OVERLAY];
  ASCanvas   *canvas;
  ASTBarData *bar;

  Window IconWin;
  Window swallowed_win;
  XSizeHints hints;
  char *swallow_command;
  char *hangon;
  char swallow;
#ifdef ENABLE_DND
  char *drop_action;
#endif
  unsigned long flags;			/* see button flags enum above */
}button_info;

enum /* folder_info flags */
{
  WF_Shaped		= (1 <<  0),	/* is the folder shaped? */
  WF_NeedTransUpdate	= (1 <<  1), 	/* do we need to update transparency
  					 * before mapping the folder? */
  WF_Mapped		= (1 <<  2)	/* is this folder mapped? */
};

typedef struct folder_info
{
  folder_info* next;
  button_info* parent;			/* button which owns this folder */
  button_info* first;			/* pointer to first button */
  int count;                /* count folded buttons */
  ASCanvas *canvas;
  Window win;         			/* Window of the Folder */
  int direction;                /* direction of the folder */
  unsigned long flags;          /* see folder flags enum above */

}folder_info;

/*************************************************************************
 *
 * Subroutine Prototypes
 *
 *************************************************************************/
/* from Wharf.c */
extern void   CreateWindow(void);
extern unsigned long  GetColor(char *name);
extern void   nocolor(char *a, char *b);
extern void   match_stringWharf(char *tline);
#ifdef ENABLE_SOUND
extern void   bind_sound(char *tline);
extern void   PlaySound(int event);
#endif
extern void   Loop(void);
extern void   ParseOptions(char *);
extern void   ParseBaseOptions(char *);
extern void   change_window_name(char *str);
extern void   DeadPipe(int nonsense);

/* from icons.c */
Window CreateButtonIconWindow (Window win);
void RenderButtonIcon (button_info * button);
//void ConfigureIconWindow(button_info* button);
extern ASImage *GetXPMData(const char **data);
extern ASImage *GetXPMGradient(int from[3], int to[3], int maxcols, int type);
extern ASImage *GetSolidXPM(ARGB32 color);
extern Bool Pushed;
extern void DrawOutline(Drawable d, int w, int h);

/* from Wharf.c */
void process_message(unsigned long type,unsigned long *body);
void send_clientmessage (Window w, Atom a, Time timestamp);
void swallow(unsigned long *body);
void ConstrainSize (XSizeHints *hints, int *widthp, int *height);
void MapFolder(folder_info* folder);
void CloseFolder(folder_info* folder);
void OpenFolder(folder_info* folder);

void GenerateButtonImage(button_info * button, Bool pushed);
void GenerateFolderImages(folder_info * folder);


//void RedrawPushed(button_info* button);
//void RedrawUnpushed(button_info* button);
//void RedrawUnpushedOutline(button_info* button);
//void RedrawPushedOutline(button_info* button);
//void RedrawWindow(folder_info* folder, button_info* newbutton);
void CreateShadowGC(void);
void CreateIconPixmap(void);
void place_buttons(folder_info* folder);
void place_folders (folder_info * folder);
void update_transparency (folder_info * folder);
void update_look (folder_info * folder);
int x_error_handler (Display * dpy, XErrorEvent * error);

