/*
 * Copyright (c) 1998 Michal Vitecek <M.Vitecek@sh.cvut.cz>
 * Copyright (c) 1998,2002 Sasha Vasko <sasha at aftercode.net>
 * Copyright (C) 1998 Ethan Fischer
 * Copyright (C) 1998 Guylhem Aznar
 * Copyright (C) 1996 Alfredo K. Kojima
 * Copyright (C) 1996 Beat Christen
 * Copyright (C) 1996 Kaj Groner
 * Copyright (C) 1996 Frank Fejes
 * Copyright (C) 1996 mj@dfv.rwth-aachen.de
 * Copyright (C) 1995 Bo Yang
 * Copyright (C) 1993 Robert Nation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define DO_CLOCKING
#define LOCAL_DEBUG
#define EVENT_TRACE

#include "../../configure.h"

#include "../../include/asapp.h"
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/time.h>
#define IN_MODULE
#define MODULE_X_INTERFACE

#include "../../include/afterstep.h"
#include "../../include/screen.h"
#include "../../include/session.h"
#include "../../include/module.h"
#include "../../include/parse.h"
#include "../../include/parser.h"
#include "../../include/confdefs.h"
#include "../../include/mystyle.h"
#include "../../include/mystyle_property.h"
#include "../../include/balloon.h"
#include "../../include/aswindata.h"
#include "../../include/decor.h"
#include "../../include/event.h"
#include "../../include/wmprops.h"
#include "../../include/functions.h"

#ifdef ENABLE_SOUND
#define WHEV_PUSH		0
#define WHEV_CLOSE_FOLDER	1
#define WHEV_OPEN_FOLDER	2
#define WHEV_CLOSE_MAIN		3
#define WHEV_OPEN_MAIN		4
#define WHEV_DROP		5
#define MAX_EVENTS		6
#endif

#define MAGIC_WHARF_BUTTON    0xA38AB110
#define MAGIC_WHARF_FOLDER    0xA38AF01D

struct ASWharfFolder;

typedef struct ASSwallowed
{
    ASCanvas *normal, *iconic;
    ASCanvas *current;                         /* one of the above */
}ASSwallowed;

typedef struct ASWharfButton
{
    unsigned long magic;
#define ASW_SwallowTarget   (0x01<<0)
#define ASW_MaxSwallow      (0x01<<1)
    ASFlagType flags;
    char        *name ;
    ASCanvas    *canvas;
    ASSwallowed *swallowed;
    ASTBarData  *bar;

    unsigned int     desired_width, desired_height;
    /* this is where it it will actually be placed and what size it should have at the end : */
    int              folder_x, folder_y;
    unsigned int     folder_width, folder_height;

    FunctionData    *fdata;

    struct ASWharfFolder   *folder;
    struct ASWharfFolder   *parent;
}ASWharfButton;

typedef struct ASWharfFolder
{
    unsigned long magic;
#define ASW_Mapped          (0x01<<0)
#define ASW_Vertical        (0x01<<1)
#define ASW_Withdrawn       (0x01<<2)
#define ASW_NeedsShaping    (0x01<<3)
#define ASW_Shaped          (0x01<<4)
    ASFlagType  flags;

    ASCanvas    *canvas;
    ASWharfButton *buttons ;
    int buttons_num;
    ASWharfButton *parent ;

    unsigned int total_width, total_height;    /* size calculated based on size of participating buttons */

    int animation_steps;                       /* how many steps left */
    int animation_dir;                         /* +1 or -1 */
}ASWharfFolder;

typedef struct ASWharfState
{
    ASHashTable   *win2obj_xref;               /* xref of window IDs to wharf buttons and folders */
    ASWharfFolder *root_folder ;

    ASHashTable   *swallow_targets;            /* hash of buttons that needs to swallow  */

    ASWharfButton *pressed_button;
    int pressed_state;

    Bool shaped_style;

}ASWharfState;

ASWharfState WharfState;
WharfConfig *Config = NULL;

#define WHARF_BUTTON_EVENT_MASK   (ButtonReleaseMask |\
                                   ButtonPressMask | LeaveWindowMask | EnterWindowMask |\
                                   StructureNotifyMask)
#define WHARF_FOLDER_EVENT_MASK   (StructureNotifyMask)


void HandleEvents();
void process_message (unsigned long type, unsigned long *body);
void DispatchEvent (ASEvent * Event);
Window make_wharf_window();
void GetOptions (const char *filename);
void GetBaseOptions (const char *filename);
void CheckConfigSanity();

ASWharfFolder *build_wharf_folder( WharfButton *list, ASWharfButton *parent, Bool vertical );
Bool display_wharf_folder( ASWharfFolder *aswf, int left, int top, int right, int bottom );
Bool display_main_folder();
void on_wharf_moveresize( ASEvent *event );
void destroy_wharf_folder( ASWharfFolder **paswf );
void on_wharf_pressed( ASEvent *event );
void release_pressure();
Bool check_pending_swallow( ASWharfFolder *aswf );
void exec_pending_swallow( ASWharfFolder *aswf );
void check_swallow_window( ASWindowData *wd );


/***********************************************************************
 *   main - start of module
 ***********************************************************************/
int
main (int argc, char **argv)
{
    /* Save our program name - for error messages */
    InitMyApp (CLASS_WHARF, argc, argv, NULL, NULL, 0 );

    memset( &WharfState, 0x00, sizeof(WharfState));

    ConnectX( &Scr, PropertyChangeMask );
    ConnectAfterStep (M_TOGGLE_PAGING |
                    M_NEW_DESK |
                    M_END_WINDOWLIST |
                    WINDOW_CONFIG_MASK |
                    WINDOW_NAME_MASK);

    Config = CreateWharfConfig ();

    LOCAL_DEBUG_OUT("parsing Options ...%s","");
    LoadBaseConfig (GetBaseOptions);
    LoadConfig ("wharf", GetOptions);

    CheckConfigSanity();

    WharfState.root_folder = build_wharf_folder( Config->root_folder, NULL, (Config->columns > 0 ) );
    if( !display_main_folder() )
    {
        show_error( "main folder does not have any entries or has zero size. Aborting!");
        return 1;
    }

    /* Create a list of all windows */
    /* Request a list of all windows,
     * wait for ConfigureWindow packets */
    if( check_pending_swallow(WharfState.root_folder) )
        SendInfo ("Send_WindowList", 0);

    /* create main folder here : */

    LOCAL_DEBUG_OUT("starting The Loop ...%s","");
    HandleEvents();

    return 0;
}

void HandleEvents()
{
    ASEvent event;
    Bool has_x_events = False ;
    while (True)
    {
        while((has_x_events = XPending (dpy)))
        {
            if( ASNextEvent (&(event.x), True) )
            {
                event.client = NULL ;
                setup_asevent_from_xevent( &event );
                DispatchEvent( &event );
            }
        }
        module_wait_pipes_input (process_message );
    }
}

void
DeadPipe (int nonsense)
{
    destroy_wharf_folder( &(WharfState.root_folder) );
    DestroyWharfConfig( Config );
    destroy_ashash( &(WharfState.win2obj_xref) );
    window_data_cleanup();

    FreeMyAppResources();
#ifdef DEBUG_ALLOCS
/* normally, we let the system clean up, but when auditing time comes
 * around, it's best to have the books in order... */
    print_unfreed_mem ();
#endif /* DEBUG_ALLOCS */
    XFlush (dpy);			/* need this for SetErootPixmap to take effect */
	XCloseDisplay (dpy);		/* need this for SetErootPixmap to take effect */
    exit (0);
}


/*****************************************************************************
 *
 * This routine is responsible for reading and parsing the config file
 *
 ****************************************************************************/
void
CheckConfigSanity()
{
    char buf[256];

    if( Config == NULL )
        Config = CreateWharfConfig ();

    if( Config->rows <= 0 && Config->columns <= 0 )
        Config->rows = 1;

    if( get_flags( Config->set_flags, WHARF_FORCE_SIZE ) )
    {
        if( Config->force_size.width == 0 )
            Config->force_size.width = 64 ;
        if( Config->force_size.height == 0 )
            Config->force_size.height = 64 ;
    }

    mystyle_get_property (Scr.wmprops);

    sprintf( buf, "*%sTile", get_application_name() );
    LOCAL_DEBUG_OUT("Attempting to use style \"%s\"", buf);
    Scr.Look.MSWindow[BACK_UNFOCUSED] = mystyle_find_or_default( buf );
    LOCAL_DEBUG_OUT("Will use style \"%s\"", Scr.Look.MSWindow[BACK_UNFOCUSED]->name);
    if( !get_flags( Config->set_flags, WHARF_FORCE_SIZE ) )
    {
        if( Scr.Look.MSWindow[BACK_UNFOCUSED]->back_icon.image != NULL &&
            !get_flags(Config->flags, WHARF_FIT_CONTENTS))
        {
            Config->force_size.width = Scr.Look.MSWindow[BACK_UNFOCUSED]->back_icon.width ;
            Config->force_size.height = Scr.Look.MSWindow[BACK_UNFOCUSED]->back_icon.height ;
        }else
        {
            Config->force_size.width = 0;
            Config->force_size.height = 0;
        }
    }

    WharfState.shaped_style = False ;
    if( Scr.Look.MSWindow[BACK_UNFOCUSED]->texture_type >= TEXTURE_TEXTURED_START &&
        Scr.Look.MSWindow[BACK_UNFOCUSED]->texture_type <= TEXTURE_SHAPED_PIXMAP  )
    {
        WharfState.shaped_style = True;
        LOCAL_DEBUG_OUT( "shaped pixmap detected%s","");
    }else if( Scr.Look.MSWindow[BACK_UNFOCUSED]->texture_type >= TEXTURE_SCALED_PIXMAP &&
        Scr.Look.MSWindow[BACK_UNFOCUSED]->texture_type <= TEXTURE_PIXMAP  )
    {
        ASImage *im = Scr.Look.MSWindow[BACK_UNFOCUSED]->back_icon.image ;
        if( im && check_asimage_alpha( Scr.asv, im ) )
        {
            WharfState.shaped_style = True ;
            LOCAL_DEBUG_OUT( "transparent pixmap detected%s","");
        }
    }

    show_progress( "printing wharf config : ");
    PrintWharfConfig(Config);
}

void
merge_wharf_folders( WharfButton **pf1, WharfButton **pf2 )
{
    while( *pf1 )
        pf1 = &((*pf1)->next);
    *pf1 = *pf2 ;
    *pf2 = NULL ;
}

void
GetOptions (const char *filename)
{
    WharfConfig *config;
    START_TIME(option_time);
SHOW_CHECKPOINT;
    show_progress( "loading wharf config from \"%s\": ", filename);
    config = ParseWharfOptions (filename, MyName);
    SHOW_TIME("Config parsing",option_time);

    /* Need to merge new config with what we have already :*/
    /* now lets check the config sanity : */
    /* mixing set and default flags : */
    Config->flags = (config->flags&config->set_flags)|(Config->flags & (~config->set_flags));
    Config->set_flags |= config->set_flags;

    if( get_flags(config->set_flags, WHARF_ROWS) )
        Config->rows = config->rows;

    if( get_flags(config->set_flags, WHARF_COLUMNS) )
        Config->columns = config->columns;

    if( get_flags( config->set_flags, WHARF_GEOMETRY ) )
        merge_geometry(&(config->geometry), &(Config->geometry) );

    if( get_flags( config->set_flags, WHARF_WITHDRAW_STYLE ) )
        Config->withdraw_style = config->withdraw_style;

    if( get_flags( config->set_flags, WHARF_FORCE_SIZE ) )
        merge_geometry( &(Config->force_size), &(config->force_size));

    if( get_flags( config->set_flags, WHARF_ANIMATE_STEPS ) )
        Config->animate_steps = config->animate_steps;
    if( get_flags( config->set_flags, WHARF_ANIMATE_STEPS_MAIN ) )
        Config->animate_steps_main = config->animate_steps_main ;
    if( get_flags( config->set_flags, WHARF_ANIMATE_DELAY ) )
        Config->animate_delay = config->animate_delay;
    if( get_flags( config->set_flags, WHARF_LABEL_LOCATION ) )
        Config->label_location = config->label_location;
    if( get_flags( config->set_flags, WHARF_ALIGN_CONTENTS ) )
        Config->align_contents = config->align_contents;

//LOCAL_DEBUG_OUT( "align_contents = %d", Config->align_contents );
    if( get_flags( config->set_flags, WHARF_SOUND ) )
    {
        int i ;
        for( i = 0 ; i < WHEV_MAX_EVENTS ; ++i )
        {
            set_string_value(&(Config->sounds[i]), config->sounds[i], NULL, 0 );
            config->sounds[i] = NULL ;
        }
    }
    /* merging folders : */

    if( config->root_folder )
        merge_wharf_folders( &(Config->root_folder), &(config->root_folder) );

    if (config->style_defs)
        ProcessMyStyleDefinitions (&(config->style_defs));

    DestroyWharfConfig (config);
    SHOW_TIME("Config parsing",option_time);
}
/*****************************************************************************
 *
 * This routine is responsible for reading and parsing the base file
 *
 ****************************************************************************/
void
GetBaseOptions (const char *filename)
{
    START_TIME(started);
    BaseConfig *config = ParseBaseOptions (filename, MyName);

    if (!config)
        exit (0);           /* something terrible has happend */

    if( Scr.image_manager )
        destroy_image_manager( Scr.image_manager, False );
    Scr.image_manager = create_image_manager( NULL, 2.2, config->pixmap_path, config->icon_path, NULL );
    if( Scr.font_manager )
        destroy_font_manager( Scr.font_manager, False );

    Scr.font_manager = create_font_manager (dpy, config->font_path, NULL);


    Scr.VScale = config->desktop_scale;

    DestroyBaseConfig (config);

    Scr.Vx = 0;
    Scr.Vy = 0;

    Scr.VxMax = Scr.MyDisplayWidth;
    Scr.VyMax = Scr.MyDisplayHeight;

    SHOW_TIME("BaseConfigParsingTime",started);
}

/****************************************************************************/
/* Window ID xref :                                                         */
/****************************************************************************/
Bool
register_object( Window w, ASMagic *obj)
{
    if( WharfState.win2obj_xref == NULL )
        WharfState.win2obj_xref = create_ashash(0, NULL, NULL, NULL);

    return (add_hash_item(WharfState.win2obj_xref, AS_HASHABLE(w), obj) == ASH_Success);
}

ASMagic *
fetch_object( Window w )
{
    ASMagic *obj = NULL ;
    if( WharfState.win2obj_xref )
        get_hash_item( WharfState.win2obj_xref, AS_HASHABLE(w), (void**)&obj );
    return obj ;
}

void
unregister_object( Window w )
{
    if( WharfState.win2obj_xref )
        remove_hash_item( WharfState.win2obj_xref, AS_HASHABLE(w), NULL, False );
}

Bool
register_swallow_target( char *name, ASWharfButton *aswb)
{
    if( name && aswb )
    {
        if( WharfState.swallow_targets == NULL )
            WharfState.swallow_targets = create_ashash(0, casestring_hash_value, casestring_compare, NULL);

        return (add_hash_item(WharfState.swallow_targets, AS_HASHABLE(name), aswb) == ASH_Success);
    }
    return False;
}

ASWharfButton *
fetch_swallow_target( char *name )
{
    ASWharfButton *aswb = NULL ;
    if( WharfState.swallow_targets && name )
        get_hash_item( WharfState.swallow_targets, AS_HASHABLE(name), (void**)&aswb );
    return aswb ;
}

void
unregister_swallow_target( char *name )
{
    if( WharfState.swallow_targets && name )
        remove_hash_item( WharfState.swallow_targets, AS_HASHABLE(name), NULL, False );
}


/****************************************************************************/
/* PROCESSING OF AFTERSTEP MESSAGES :                                       */
/****************************************************************************/
void
process_message (unsigned long type, unsigned long *body)
{
    LOCAL_DEBUG_OUT( "received message %lX", type );
	if( (type&WINDOW_PACKET_MASK) != 0 )
	{
		struct ASWindowData *wd = fetch_window_by_id( body[0] );
        WindowPacketResult res ;
        /* saving relevant client info since handle_window_packet could destroy the actuall structure */
#if defined(LOCAL_DEBUG) && !defined(NO_DEBUG_OUTPUT)
        Window               saved_w = (wd && wd->canvas)?wd->canvas->w:None;
        int                  saved_desk = wd?wd->desk:INVALID_DESK;
        struct ASWindowData *saved_wd = wd ;
#endif
        LOCAL_DEBUG_OUT( "message %lX window %lX data %p", type, body[0], wd );
        res = handle_window_packet( type, body, &wd );
        LOCAL_DEBUG_OUT( "\t res = %d, data %p", res, wd );
        if( res == WP_DataCreated || res == WP_DataChanged )
        {
            check_swallow_window( wd );
        }else if( res == WP_DataDeleted )
        {
            LOCAL_DEBUG_OUT( "client deleted (%p)->window(%lX)->desk(%d)", saved_wd, saved_w, saved_desk );
        }
    }else if( type == M_END_WINDOWLIST )
        exec_pending_swallow( WharfState.root_folder );
}
/*************************************************************************
 * Event handling :
 *************************************************************************/
void
DispatchEvent (ASEvent * event)
{
#ifndef EVENT_TRACE
    if( get_output_threshold() >= OUTPUT_LEVEL_DEBUG )
#endif
    {
        show_progress("****************************************************************");
        show_progress("%s:%s:%d><<EVENT type(%d(%s))->x.window(%lx)->event.w(%lx)->client(%p)->context(%s)->send_event(%d)", __FILE__, __FUNCTION__, __LINE__, event->x.type, event_type2name(event->x.type), event->x.xany.window, event->w, event->client, context2text(event->context), event->x.xany.send_event);
    }

    event->client = NULL ;
    balloon_handle_event (&(event->x));

    switch (event->x.type)
    {
	    case ConfigureNotify:
            on_wharf_moveresize( event );
            break;
        case KeyPress :
            break ;
        case KeyRelease :
            break ;
        case ButtonPress:
            on_wharf_pressed( event );
            break;
        case ButtonRelease:
            release_pressure();
            break;
        case MotionNotify :
            break ;
	    case ClientMessage:
            {
#ifdef LOCAL_DEBUG
                char *name = XGetAtomName( dpy, event->x.xclient.message_type );
                LOCAL_DEBUG_OUT("ClientMessage(\"%s\",data=(%lX,%lX,%lX,%lX,%lX)", name, event->x.xclient.data.l[0], event->x.xclient.data.l[1], event->x.xclient.data.l[2], event->x.xclient.data.l[3], event->x.xclient.data.l[4]);
                XFree( name );
#endif
                if ( event->x.xclient.format == 32 &&
                    event->x.xclient.data.l[0] == _XA_WM_DELETE_WINDOW )
                {
                    DeadPipe(0);
                }
            }
            break;
        case ReparentNotify :
            if( event->x.xreparent.parent == Scr.Root )
            {
                sleep_a_little( 50000 );
                XMoveResizeWindow( dpy, event->x.xreparent.window, -1000, -1000, 1, 1 );
            }
            break ;
	    case PropertyNotify:
            if( event->x.xproperty.atom == _XROOTPMAP_ID && event->w == Scr.Root )
            {
                LOCAL_DEBUG_OUT( "root background updated!%s","");
                safe_asimage_destroy( Scr.RootImage );
                Scr.RootImage = NULL ;
            }
            break;
    }
}

/*************************************************************************/
/* Wharf buttons :                                                       */

ASCanvas*
create_wharf_folder_canvas(ASWharfFolder *aswf)
{
    static XSetWindowAttributes attr ;
    Window w ;
    ASCanvas *pc = NULL;
    unsigned long mask = CWEventMask|CWBackPixmap ;

    attr.event_mask = WHARF_FOLDER_EVENT_MASK ;
    attr.background_pixel = Scr.asv->black_pixel ;
    attr.background_pixmap = ParentRelative ;
    if( Scr.asv->visual_info.visual != DefaultVisual( dpy, DefaultScreen(dpy)) )
        mask |= CWBackPixel ;
    w = create_visual_window( Scr.asv, Scr.Root, 0, 0, 1, 1, 0, InputOutput, mask, &attr );
    register_object( w, (ASMagic*)aswf );
    pc = create_ascanvas_container(w);
    LOCAL_DEBUG_OUT("folder canvas %p created for window %lX", pc, w );
    return pc;
}


ASCanvas*
create_wharf_button_canvas(ASWharfButton *aswb, ASCanvas *parent)
{
    static XSetWindowAttributes attr ;
    Window w ;
    ASCanvas *canvas;

    attr.event_mask = WHARF_BUTTON_EVENT_MASK ;
    w = create_visual_window( Scr.asv, parent->w, -1, -1, 1, 1, 0, InputOutput, CWEventMask, &attr );
    register_object( w, (ASMagic*)aswb );
    canvas = create_ascanvas(w);

    if( WharfState.shaped_style )
        set_flags( canvas->state, CANVAS_FORCE_MASK );
    return canvas;
}

ASTBarData *
build_wharf_button_tbar(WharfButton *wb)
{
    ASTBarData *bar = create_astbar();
    int icon_row = 0, icon_col = 0;
    int label_row = 0, label_col = 0;
    int label_flip = get_flags( Config->flags, WHARF_FLIP_LABEL )?FLIP_UPSIDEDOWN:0;
    int label_align = NO_ALIGN ;
#define WLL_Vertical    (0x01<<0)
#define WLL_Opposite    (0x01<<1)
#define WLL_AlignCenter (0x01<<2)
#define WLL_AlignRight  (0x01<<3)
#define WLL_OverIcon    (0x01<<4)

    if( get_flags( Config->flags, WHARF_SHOW_LABEL ) && wb->title )
    {
        if( get_flags(Config->label_location, WLL_Vertical ))
            label_flip |= FLIP_VERTICAL ;
        if( !get_flags(Config->label_location, WLL_OverIcon ))
        {
            if( get_flags(Config->label_location, WLL_Vertical ))
            {
                if( get_flags(Config->label_location, WLL_Opposite ))
                    label_col = 1;
                else
                    icon_col = 1;
            }else if( get_flags(Config->label_location, WLL_Opposite ))
                label_row = 1;
            else
                icon_row = 1;
        }
        if( get_flags(Config->label_location, WLL_AlignCenter ))
            label_align = ALIGN_CENTER ;
        else if( get_flags(Config->label_location, WLL_AlignRight ))
            label_align = ALIGN_RIGHT|ALIGN_TOP ;
        else
            label_align = ALIGN_LEFT|ALIGN_BOTTOM ;
    }
    if( wb->icon )
    {
        register int i = -1;
        while( wb->icon[++i] )
        {
            ASImage *im = NULL ;
            /* load image here */
            im = get_asimage( Scr.image_manager, wb->icon[i], ASFLAGS_EVERYTHING, 100 );
            if( im )
                add_astbar_icon( bar, icon_col, icon_row, 0, Config->align_contents, im );
        }
    }

    if( get_flags( Config->flags, WHARF_SHOW_LABEL ) && wb->title )
        add_astbar_label( bar, label_col, label_row, label_flip, label_align, wb->title );

    set_astbar_style_ptr( bar, BAR_STATE_UNFOCUSED, Scr.Look.MSWindow[BACK_UNFOCUSED] );
    if( !get_flags( Config->flags, WHARF_NO_BORDER ) )
        set_astbar_hilite( bar, RIGHT_HILITE|BOTTOM_HILITE );
    return bar;
}


/*************************************************************************/
/* Wharf folders :                                                       */

ASWharfFolder *create_wharf_folder( int button_count, ASWharfButton *parent )
{
    ASWharfFolder *aswf = NULL ;
    if( button_count > 0 )
    {
        register int i = button_count;
        aswf = safecalloc( 1, sizeof(ASWharfFolder));
        aswf->magic = MAGIC_WHARF_FOLDER ;
        aswf->buttons = safecalloc( i, sizeof(ASWharfButton));
        aswf->buttons_num = i ;
        while( --i >= 0 )
        {
            aswf->buttons[i].magic = MAGIC_WHARF_BUTTON ;
            aswf->buttons[i].parent = aswf ;
        }
        aswf->parent = parent ;
    }
    return aswf;
}

ASWharfFolder *
build_wharf_folder( WharfButton *list, ASWharfButton *parent, Bool vertical )
{
    ASWharfFolder *aswf = NULL ;
    int count = 0 ;
    WharfButton *wb = list ;
    while( wb )
    {
        ++count ;
        wb = wb->next ;
    }

    if( (aswf = create_wharf_folder(count, parent)) != NULL  )
    {
        ASWharfButton *aswb = aswf->buttons;
        aswf->canvas = create_wharf_folder_canvas(aswf);
        if( vertical )
            set_flags( aswf->flags, ASW_Vertical );
        else
            clear_flags( aswf->flags, ASW_Vertical );

        wb = list;
        while( wb )
        {
            aswb->name = mystrdup( wb->title );
            if( wb->function )
            {
                aswb->fdata = safecalloc( 1, sizeof(FunctionData) );
                dup_func_data( aswb->fdata, wb->function );
                if( IsSwallowFunc(aswb->fdata->func) )
                {
                    set_flags( aswb->flags, ASW_SwallowTarget );
                    register_swallow_target( aswb->fdata->name, aswb );
                    if( aswb->fdata->func == F_MaxSwallow ||
                        aswb->fdata->func == F_MaxSwallowModule )
                        set_flags( aswb->flags, ASW_MaxSwallow );
                }
            }

            aswb->canvas = create_wharf_button_canvas(aswb, aswf->canvas);
            aswb->bar = build_wharf_button_tbar(wb);

            if( !get_flags( aswb->flags, ASW_SwallowTarget ) )
            {
                aswb->desired_width  = (Config->force_size.width == 0)?calculate_astbar_width( aswb->bar ) :Config->force_size.width;
                aswb->desired_height = (Config->force_size.height== 0)?calculate_astbar_height( aswb->bar ):Config->force_size.height;
            }else
            {
                aswb->desired_width  = Config->force_size.width ;
                aswb->desired_height = Config->force_size.height ;
            }

            if( aswb->desired_width == 0 )
                aswb->desired_width = 64 ;

            if( aswb->desired_height == 0 )
                aswb->desired_height = 64 ;

            if( wb->folder )
                aswb->folder = build_wharf_folder( wb->folder, aswb, vertical?False:True );

            ++aswb;
            wb = wb->next ;
        }
        XMapSubwindows( dpy, aswf->canvas->w );
    }
    return aswf;
}

void
destroy_wharf_folder( ASWharfFolder **paswf )
{
    ASWharfFolder *aswf ;
    if( paswf == NULL )
        return;

    if( (aswf = *paswf) == NULL )
        return;
    if( aswf->magic != MAGIC_WHARF_FOLDER )
        return;

    if( aswf->canvas )
        unmap_canvas_window( aswf->canvas );

    if( aswf->buttons )
    {
        int i = aswf->buttons_num;
        while( --i >= 0 )
        {
            ASWharfButton *aswb = &(aswf->buttons[i]);
            if( aswb->name )
                free( aswb->name );
            destroy_astbar(&(aswb->bar));
            if( aswb->canvas )
                unregister_object( aswb->canvas->w );
            destroy_ascanvas(&(aswb->canvas));
            destroy_func_data(&(aswb->fdata));
            destroy_wharf_folder(&(aswb->folder));
        }
        free( aswf->buttons );
    }
    if( aswf->canvas )
        unregister_object( aswf->canvas->w );
    destroy_ascanvas(&(aswf->canvas));

    memset(aswf, 0x00, sizeof(ASWharfFolder));
    free( aswf );
    *paswf = NULL ;
}

inline void
update_wharf_folder_shape( ASWharfFolder *aswf )
{
#ifdef SHAPE
    int i =  aswf->buttons_num;
    int set = 0 ;

    clear_flags( aswf->flags, ASW_Shaped);

    if( (get_flags( Config->flags, WHARF_SHAPE_TO_CONTENTS ) && get_flags( aswf->flags, ASW_NeedsShaping))||
         WharfState.shaped_style )
    {
        while ( --i >= 0 )
        {
            register ASWharfButton *aswb = &(aswf->buttons[i]);
            if( aswb->canvas->width == aswb->folder_width && aswb->canvas->height == aswb->folder_height )
                if( combine_canvas_shape_at (aswf->canvas, aswb->canvas, aswb->folder_x, aswb->folder_y, (set==0) ) )
                    ++set;
            if( aswb->swallowed )
            {
                if( combine_canvas_shape_at (aswf->canvas, aswb->swallowed->current, aswb->folder_x, aswb->folder_y, (set==0) ) )
                    ++set;
            }
        }
        if( set > 0 )
            set_flags( aswf->flags, ASW_Shaped);
    }
#endif
}


void
map_wharf_folder( ASWharfFolder *aswf,
                  int x, int y, unsigned int width, unsigned int height,
                  int gravity )
{
    XSizeHints      shints;
	ExtendedWMHints extwm_hints ;
    ASFlagType protocols = 0;

    if( aswf->animation_steps > 0 )
    {/* we must make sure that we receive first ConfigureNotify !! */
        handle_canvas_config( aswf->canvas );
        if( aswf->canvas->root_x == x && aswf->canvas->root_y == y &&
            aswf->canvas->width == width && aswf->canvas->height == height )
        {
            if( get_flags( aswf->flags, ASW_Vertical ) )
                ++height ;
            else
                ++width ;
        }
    }
    moveresize_canvas( aswf->canvas, x, y, width, height );

    shints.flags = USSize|PWinGravity;
    if( get_flags( Config->set_flags, WHARF_GEOMETRY ) )
        shints.flags |= USPosition ;
    else
        shints.flags |= PPosition ;

    shints.win_gravity = gravity ;

	extwm_hints.pid = getpid();
    extwm_hints.flags = EXTWM_PID|EXTWM_StateSkipTaskbar|EXTWM_TypeDock ;

    if( aswf != WharfState.root_folder )
        XSetTransientForHint(dpy, aswf->canvas->w, WharfState.root_folder->canvas->w);
    else
        protocols = AS_DoesWmDeleteWindow ;

    set_client_hints( aswf->canvas->w, NULL, &shints, AS_DoesWmDeleteWindow, &extwm_hints );

	/* showing window to let user see that we are doing something */
    map_canvas_window(aswf->canvas, True);
    LOCAL_DEBUG_OUT( "mapping main window at %ux%u%+d%+d", width, height,  x, y );
    /* final cleanup */
    ASSync( False );
    sleep_a_little (10000);                                 /* we have to give AS a chance to spot us */
}

void
place_wharf_buttons( ASWharfFolder *aswf, int *total_width_return, int *total_height_return )
{
    int max_width = 0, max_height = 0 ;
    int x = 0, y = 0 ;
    int i;
    Bool fit_contents = get_flags(Config->flags, WHARF_FIT_CONTENTS);
    Bool needs_shaping = False ;

    *total_width_return  = 0 ;
    *total_height_return = 0 ;

    if( get_flags( aswf->flags, ASW_Vertical ) )
    {
        int columns = (aswf == WharfState.root_folder)?Config->columns:1;
        int buttons_per_column = (aswf->buttons_num + columns - 1) / columns, bc = 0 ;

        for( i = 0 ; i < aswf->buttons_num ; ++i )
        {
            ASWharfButton *aswb = &(aswf->buttons[i]);
            int height ;

            if( bc == 0 )
            {
                register int k = i+buttons_per_column ;
                if( k > aswf->buttons_num )
                    k = aswf->buttons_num ;
                max_width = 0;
                max_height = 0;
                while( --k >= i )
                {
                    register ASWharfButton *aswb = &(aswf->buttons[k]);
                    if( max_width < aswb->desired_width )
                        max_width = aswb->desired_width ;
                    if( max_height < aswb->desired_height && !get_flags( aswb->flags, ASW_MaxSwallow ) )
                        max_height = aswb->desired_height ;
                }
                LOCAL_DEBUG_OUT( "max_size(%dx%d)", max_width, max_height );
            }
            height = (get_flags( aswb->flags, ASW_MaxSwallow ) || fit_contents )?aswb->desired_height:max_height ;
            if( get_flags(Config->flags, WHARF_SHAPE_TO_CONTENTS) )
            {
                int dx = max_width - aswb->desired_width ;
                int dy = height - aswb->desired_height ;
                if( get_flags( Config->align_contents, ALIGN_RIGHT ) == 0 )
                    dx = 0 ;
                else if( get_flags( Config->align_contents, ALIGN_LEFT ))
                    dx = dx>>1 ;
                if( get_flags( Config->align_contents, ALIGN_BOTTOM ) == 0 )
                    dy = 0 ;
                else if( get_flags( Config->align_contents, ALIGN_TOP ))
                    dy = dy>>1 ;
                aswb->folder_x = x+dx ;
                aswb->folder_y = y+dy;
                aswb->folder_width = aswb->desired_width ;
                aswb->folder_height = aswb->desired_height;
                if( aswb->desired_width != max_width )
                    needs_shaping = True;
            }else
            {
                aswb->folder_x = x;
                aswb->folder_y = y;
                aswb->folder_width = max_width ;
                aswb->folder_height = height;
            }
            moveresize_canvas( aswb->canvas, aswb->folder_x, aswb->folder_y, aswb->folder_width, aswb->folder_height );
            y += height ;
            if( ++bc >= buttons_per_column )
            {
                *total_width_return += max_width ;
                if( *total_height_return < y )
                    *total_height_return = y ;
                x += max_width ;
                y = 0 ;
                bc = 0;
            }
        }
        if( columns * buttons_per_column > aswf->buttons_num )
        {
            *total_width_return += max_width ;
            if( *total_height_return < y )
                *total_height_return = y ;
        }

    }else
    {
        int rows = (aswf == WharfState.root_folder)?Config->rows:1;
        int buttons_per_row = (aswf->buttons_num + rows - 1) / rows, br = 0 ;

        for( i = 0 ; i < aswf->buttons_num ; ++i )
        {
            ASWharfButton *aswb = &(aswf->buttons[i]);
            int width ;

            if( br == 0 )
            {
                register int k = i+buttons_per_row ;
                if( k > aswf->buttons_num )
                    k = aswf->buttons_num ;
                max_width = 0;
                max_height = 0;
                while( --k >= i )
                {
                    register ASWharfButton *aswb = &(aswf->buttons[k]);
                    if( max_width < aswb->desired_width && !get_flags( aswb->flags, ASW_MaxSwallow ) )
                        max_width = aswb->desired_width ;
                    if( max_height < aswb->desired_height )
                        max_height = aswb->desired_height ;
                }
                LOCAL_DEBUG_OUT( "max_size(%dx%d)", max_width, max_height );
            }

            width = (get_flags( aswb->flags, ASW_MaxSwallow )|| fit_contents )?aswb->desired_width:max_width ;
            if( get_flags(Config->flags, WHARF_SHAPE_TO_CONTENTS) )
            {
                int dx = width - aswb->desired_width ;
                int dy = max_height - aswb->desired_height ;
                if( get_flags( Config->align_contents, ALIGN_RIGHT ) == 0 )
                    dx = 0 ;
                else if( get_flags( Config->align_contents, ALIGN_LEFT ))
                    dx = dx>>1 ;
                if( get_flags( Config->align_contents, ALIGN_BOTTOM ) == 0 )
                    dy = 0 ;
                else if( get_flags( Config->align_contents, ALIGN_TOP ))
                    dy = dy>>1 ;
                aswb->folder_x = x+dx ;
                aswb->folder_y = y+dy;
                aswb->folder_width = aswb->desired_width ;
                aswb->folder_height = aswb->desired_height;
                if( aswb->desired_height != max_height )
                    needs_shaping = True;
            }else
            {
                aswb->folder_x = x ;
                aswb->folder_y = y ;
                aswb->folder_width = width ;
                aswb->folder_height = max_height;
            }
            moveresize_canvas( aswb->canvas, aswb->folder_x, aswb->folder_y, aswb->folder_width, aswb->folder_height );
            x += width;
            if( ++br >= buttons_per_row )
            {
                if( *total_width_return < x )
                    *total_width_return = x ;
                *total_height_return += max_height ;
                x = 0 ;
                y += max_height ;
                br = 0;
            }
        }
        if( rows * buttons_per_row > aswf->buttons_num )
        {
            if( *total_width_return < x )
                *total_width_return = x ;
            *total_height_return += max_height ;
        }
    }
    LOCAL_DEBUG_OUT( "total_size_return(%dx%d)", *total_width_return, *total_height_return );

    ASSync( False );
    if( needs_shaping )
        set_flags( aswf->flags, ASW_NeedsShaping );
    else
        clear_flags( aswf->flags, ASW_NeedsShaping );
}

void
animate_wharf( ASWharfFolder *aswf, int *new_width_return, int *new_height_return )
{
    int new_width = aswf->canvas->width ;
    int new_height = aswf->canvas->height ;

    if( aswf->animation_dir < 0 )
    {
        if( get_flags( aswf->flags, ASW_Vertical ) )
        {
            if( aswf->animation_steps <= 1 )
                new_height = 0 ;
            else
            {
                new_height = (new_height*(aswf->animation_steps-1))/aswf->animation_steps ;
                if( new_height == aswf->canvas->height )
                    --new_height ;
            }
        }else
        {
            if( aswf->animation_steps <= 1 )
                new_width =0 ;
            else
            {
                new_width = (new_width*(aswf->animation_steps-1))/aswf->animation_steps;
                if( new_width == aswf->canvas->width )
                    --new_width;
            }
        }
    }else
    {
        new_width = aswf->total_width ;
        new_height = aswf->total_height ;
        if( get_flags( aswf->flags, ASW_Vertical ) )
        {
            if( aswf->animation_steps <= 1 )
                new_height = aswf->total_height ;
            else
            {
                new_height = aswf->canvas->height+((aswf->total_height - aswf->canvas->height)/aswf->animation_steps)  ;
                if( new_height == aswf->canvas->height && new_height < aswf->total_height )
                    ++new_height;
            }
        }else
        {
            if( aswf->animation_steps <= 1 )
                new_width = aswf->total_width ;
            else
            {
                new_width = aswf->canvas->width+((aswf->total_width - aswf->canvas->width)/aswf->animation_steps)  ;
                if( new_width == aswf->canvas->width && new_width < aswf->total_width )
                    ++new_width;
            }
        }
    }
    --(aswf->animation_steps);

    *new_width_return = new_width ;
    *new_height_return = new_height ;
}

Bool
display_wharf_folder( ASWharfFolder *aswf, int left, int top, int right, int bottom )
{
    Bool east  = get_flags( Config->geometry.flags, XNegative);
    Bool south = get_flags( Config->geometry.flags, YNegative);
    int x, y, width = 0, height = 0;
    int total_width = 0, total_height = 0;
    if( AS_ASSERT( aswf ) ||
        (get_flags( aswf->flags, ASW_Mapped ) && !get_flags( aswf->flags, ASW_Withdrawn )) )
        return False;

    place_wharf_buttons( aswf, &total_width, &total_height );

    if( total_width == 0 || total_height == 0 )
        return False;;
    if( total_width > Scr.MyDisplayWidth )
        total_width = Scr.MyDisplayWidth;
    if( total_height > Scr.MyDisplayHeight )
        total_height = Scr.MyDisplayHeight;

    aswf->total_width = total_width ;
    aswf->total_height = total_height ;

    if( left < 0 )
        east = False;
    else if( left < total_width )
        east = !( Scr.MyDisplayWidth > right && (Scr.MyDisplayWidth-right > total_width || left < Scr.MyDisplayWidth-right));

    if( east )
    {
        if( left < total_width )
            left = total_width ;
    }else if( Scr.MyDisplayWidth-right  < total_width )
        right = Scr.MyDisplayWidth - total_width ;

    if( south )
    {
        if( top < total_height )
            top = total_height ;
    }else if( Scr.MyDisplayHeight - bottom < total_height )
        bottom = Scr.MyDisplayHeight - total_width ;

    if( south && top < total_height )
        south = !( Scr.MyDisplayHeight > bottom && (Scr.MyDisplayHeight-bottom > total_height || top < Scr.MyDisplayHeight-bottom));

    if( !get_flags(Config->flags, WHARF_ANIMATE ) )
    {
        aswf->animation_steps = 0 ;
        aswf->animation_dir = 0 ;
    }else
    {
        if( get_flags( Config->set_flags, WHARF_ANIMATE_STEPS ) )
            aswf->animation_steps = Config->animate_steps;
        else
            aswf->animation_steps = 12 ;
    }

    if( aswf->animation_steps > 0 )
    {
        aswf->animation_dir = 1;
        aswf->canvas->width = aswf->canvas->height = 1 ;
        animate_wharf( aswf, &width, &height );
    }
    if( width == 0 )
        width = total_width ;
    if( height == 0 )
        height = total_height ;
    LOCAL_DEBUG_OUT( "animation_steps(%d)->size(%dx%d)->total_size(%dx%d)", aswf->animation_steps, width, height, total_width, total_height );

    if( get_flags( aswf->flags, ASW_Vertical ) )
    {
        x = east? right - width: left ;
        y = south? top - height : bottom ;
        if( top != bottom )
            y += south?5:-5 ;
    }else
    {
        x = east? left - width : right ;
        y = south? bottom - height: top ;
        if( left != right)
            x += east?5:-5 ;
    }
    update_wharf_folder_shape( aswf );
    map_wharf_folder( aswf, x, y, width, height, east?(south?SouthEastGravity:NorthEastGravity):
                                                      (south?SouthWestGravity:NorthWestGravity) );
    set_flags( aswf->flags, ASW_Mapped );
    clear_flags( aswf->flags, ASW_Withdrawn );
    return True;
}

static inline void
unmap_wharf_folder( ASWharfFolder *aswf )
{
    int i = aswf->buttons_num;
LOCAL_DEBUG_OUT( "unmapping canvas %p at %dx%d%+d%+d", aswf->canvas, aswf->canvas->width, aswf->canvas->height, aswf->canvas->root_x, aswf->canvas->root_y );
    unmap_canvas_window( aswf->canvas );
    moveresize_canvas( aswf->canvas, -1000, -1000, 1, 1 );/* to make sure we get ConfigureNotify next time we map the folder again */
    clear_flags( aswf->flags, ASW_Mapped );

    while ( --i >= 0 )
    {
        if( aswf->buttons[i].folder &&
            get_flags( aswf->buttons[i].folder->flags, ASW_Mapped ) )
            unmap_wharf_folder( aswf->buttons[i].folder );
    }
}

void
withdraw_wharf_folder( ASWharfFolder *aswf )
{
LOCAL_DEBUG_OUT( "withdrawing folder %p", aswf );
    if( AS_ASSERT(aswf) )
        return;
LOCAL_DEBUG_OUT( "folder->flags(%lX)", aswf->flags );
    if( !get_flags( aswf->flags, ASW_Mapped ) )
        return ;

    if( !get_flags(Config->flags, WHARF_ANIMATE ) )
    {
LOCAL_DEBUG_OUT( "unmapping folder %p", aswf );
        unmap_wharf_folder( aswf );
    }else
    {
        if( get_flags( Config->set_flags, WHARF_ANIMATE_STEPS ) )
            aswf->animation_steps = Config->animate_steps;
        else
            aswf->animation_steps = 12 ;
        if( aswf->animation_steps <= 1 )
        {
            unmap_wharf_folder( aswf );
LOCAL_DEBUG_OUT( "no animations left - unmapping folder %p", aswf );
        }else
        {
            int new_width = aswf->canvas->width ;
            int new_height = aswf->canvas->height ;

            aswf->animation_dir = -1;
            animate_wharf( aswf, &new_width, &new_height );
LOCAL_DEBUG_OUT( "animating folder %p to %dx%d", aswf, new_width, new_height );
            if( new_width == 0 || new_height == 0 )
                unmap_wharf_folder( aswf );
            else
            {
                int i = aswf->buttons_num;
                while ( --i >= 0 )
                {
                    if( aswf->buttons[i].folder &&
                        get_flags( aswf->buttons[i].folder->flags, ASW_Mapped ) )
                    unmap_wharf_folder( aswf->buttons[i].folder );
                }
            }
            resize_canvas( aswf->canvas, new_width, new_height );
        }
    }
    ASSync( False );
}

Bool display_main_folder()
{
    int left = Config->geometry.x;
    int top = Config->geometry.y;

    if( get_flags( Config->geometry.flags, XNegative ))
        left += Scr.MyDisplayWidth ;

    if( get_flags( Config->geometry.flags, YNegative ))
        top += Scr.MyDisplayHeight ;
    return display_wharf_folder( WharfState.root_folder, left, top, left, top );
}

Bool
check_pending_swallow( ASWharfFolder *aswf )
{
    if( aswf )
    {
        int i = aswf->buttons_num ;
        /* checking buttons : */
        while( --i >= 0 )
            if( get_flags(aswf->buttons[i].flags, ASW_SwallowTarget ) )
                return True;
        /* checking subfolders : */
        i = aswf->buttons_num ;
        while( --i >= 0 )
            if( aswf->buttons[i].folder )
                if( check_pending_swallow( aswf->buttons[i].folder ) )
                    return True;
    }
    return False;
}

void
exec_pending_swallow( ASWharfFolder *aswf )
{
    if( aswf )
    {
        int i = aswf->buttons_num ;
        while( --i >= 0 )
        {
            if( get_flags(aswf->buttons[i].flags, ASW_SwallowTarget ) &&
                aswf->buttons[i].fdata &&
                aswf->buttons[i].swallowed == NULL )
            {
                SendCommand( aswf->buttons[i].fdata, 0);
            }
            if( aswf->buttons[i].folder )
                exec_pending_swallow( aswf->buttons[i].folder );
        }
    }
}

void
grab_swallowed_canvas_btns( ASCanvas *canvas, Bool action, Bool withdraw )
{
    unsigned *mods = lock_mods;
    do
    {
        /* grab button 1 if this button performs an action */
        if( action )
            XGrabButton (dpy, Button1, *mods,
                        canvas->w,
                        False, ButtonPressMask | ButtonReleaseMask,
                        GrabModeAsync, GrabModeAsync, None, None);
        /* grab button 3 if this is the root folder */
        if (withdraw )
            XGrabButton (dpy, Button3, *mods,
                        canvas->w,
                        False, ButtonPressMask | ButtonReleaseMask,
                        GrabModeAsync, GrabModeAsync, None, None);
    }while (*mods++);
}

void
check_swallow_window( ASWindowData *wd )
{
    ASWharfButton *aswb = NULL ;
    Window w;
    unsigned int total_width = 1, total_height = 1;

    if( wd == NULL && !get_flags( wd->state_flags, AS_Mapped))
        return;
    LOCAL_DEBUG_OUT( "name(\"%s\")->icon_name(\"%s\")->res_class(\"%s\")->res_name(\"%s\")",
                     wd->window_name, wd->icon_name, wd->res_class, wd->res_name );
    if( (aswb = fetch_swallow_target( wd->window_name )) == NULL )
        if( (aswb = fetch_swallow_target( wd->icon_name )) == NULL )
            if( (aswb = fetch_swallow_target( wd->res_class )) == NULL )
                if( (aswb = fetch_swallow_target( wd->res_name )) == NULL )
                    return ;
    LOCAL_DEBUG_OUT( "swallow target is %p, swallowed = %p", aswb, aswb->swallowed );
    if( aswb->swallowed != NULL )
        return;
    /* do the actuall swallowing here : */
    XGrabServer( dpy );
    /* first lets check if window is still not swallowed : it should have no more then 2 parents before root */
    w = get_parent_window( wd->client );
    LOCAL_DEBUG_OUT( "first parent %lX, root %lX", w, Scr.Root );
    if( w != Scr.Root && w != None )
        w = get_parent_window( w );
    LOCAL_DEBUG_OUT( "second parent %lX, root %lX", w, Scr.Root );
    if( w == Scr.Root )
    {
        ASWharfFolder *aswf = aswb->parent ;
        Bool withdraw_btn = (WITHDRAW_ON_EDGE(Config) && (&(aswf->buttons[0]) == aswb || &(aswf->buttons[aswf->buttons_num-1]) == aswb)) ||
                            WITHDRAW_ON_ANY(Config) ;
        /* its ok - we can swallow it now : */
        /* create swallow object : */
        aswb->swallowed = safecalloc( 1, sizeof(ASSwallowed ));
        /* first thing - we reparent window and its icon if there is any */
        XReparentWindow( dpy, wd->client, aswb->canvas->w, 0, 0 );
        aswb->swallowed->normal = create_ascanvas_container( wd->client );
        register_object( wd->client, (ASMagic*)aswb );
        XSelectInput (dpy, wd->client, StructureNotifyMask);
        grab_swallowed_canvas_btns( aswb->swallowed->normal,
                                    (aswb->folder!=NULL),
                                    withdraw_btn && aswb->parent == WharfState.root_folder);
        if( get_flags( wd->flags, AS_ClientIcon ) && !get_flags( wd->flags, AS_ClientIconPixmap))
        {
            XReparentWindow( dpy, wd->icon, aswb->canvas->w, 0, 0 );
            aswb->swallowed->iconic = create_ascanvas_container( wd->icon );
            register_object( wd->icon, (ASMagic*)aswb );
            XSelectInput (dpy, wd->icon, StructureNotifyMask);
            grab_swallowed_canvas_btns(  aswb->swallowed->iconic,
                                        (aswb->folder!=NULL),
                                        withdraw_btn && aswb->parent == WharfState.root_folder);
        }
        aswb->swallowed->current = ( get_flags( wd->state_flags, AS_Iconic ) &&
                                     aswb->swallowed->iconic != NULL )?
                                   aswb->swallowed->iconic:aswb->swallowed->normal;
        handle_canvas_config( aswb->swallowed->current );
        LOCAL_DEBUG_OUT( "client(%lX)->icon(%lX)->current(%lX)", wd->client, wd->icon, aswb->swallowed->current->w );

        if( get_flags( aswb->flags, ASW_MaxSwallow ) || Config->force_size.width == 0 )
            aswb->desired_width = aswb->swallowed->current->width;
        if( get_flags( aswb->flags, ASW_MaxSwallow ) || Config->force_size.height == 0 )
            aswb->desired_height = aswb->swallowed->current->height;

        resize_canvas( aswb->swallowed->current, aswb->desired_width, aswb->desired_height );
        map_canvas_window( aswb->swallowed->current, True );

        place_wharf_buttons( aswf, &total_width, &total_height );

        if( total_width != 0 && total_height != 0 )
        {
            if( total_width > Scr.MyDisplayWidth )
                total_width = Scr.MyDisplayWidth;
            if( total_height > Scr.MyDisplayHeight )
                total_height = Scr.MyDisplayHeight;

            aswf->total_width = total_width ;
            aswf->total_height = total_height ;
        }
        resize_canvas( aswf->canvas, total_width, total_height );
    }
    ASSync(False);
    XUngrabServer( dpy );
}

/*************************************************************************/
/* Event handling                                                        */
/*************************************************************************/
Bool
on_wharf_button_moveresize( ASWharfButton *aswb, ASEvent *event )
{
    ASFlagType changes = handle_canvas_config (aswb->canvas );
    ASFlagType swallowed_changes = 0 ;

    if( aswb->folder_width != aswb->canvas->width ||
        aswb->folder_height != aswb->canvas->height )
        return False;

    if( get_flags(changes, CANVAS_RESIZED ) )
        set_astbar_size( aswb->bar, aswb->canvas->width, aswb->canvas->height );
    else if( get_flags(changes, CANVAS_MOVED ) )
        update_astbar_transparency( aswb->bar, aswb->canvas );

    if( changes != 0 && (DoesBarNeedsRendering(aswb->bar) || is_canvas_needs_redraw( aswb->canvas )))
        render_astbar( aswb->bar, aswb->canvas );

    if( aswb->swallowed )
    {
        swallowed_changes = handle_canvas_config (aswb->swallowed->current );
    }
#ifndef SHAPE
    swallowed_changes = 0 ;                    /* if no shaped extentions - ignore the changes */
#endif
    if( is_canvas_dirty( aswb->canvas ) || swallowed_changes != 0 )
    {
        update_canvas_display( aswb->canvas );
#ifdef SHAPE
        if( aswb->canvas->mask != None && aswb->swallowed )
        {
            combine_canvas_shape_at (aswb->canvas, aswb->swallowed->current, 0, 0, False );
        }
#endif
    }
    return True;
}

void on_wharf_moveresize( ASEvent *event )
{
    ASMagic *obj = NULL;

    obj = fetch_object( event->w ) ;
    if( obj == NULL )
        return;
    if( obj->magic == MAGIC_WHARF_BUTTON )
    {
        ASWharfButton *aswb = (ASWharfButton*)obj;
        if( on_wharf_button_moveresize( aswb, event ) )
        {
#ifdef SHAPE
            if( get_flags( aswb->parent->flags, ASW_Shaped) )
            {
                replace_canvas_shape_at( aswb->parent->canvas, aswb->canvas, aswb->folder_x, aswb->folder_y );
                if( aswb->canvas->mask != None && aswb->swallowed )
                    combine_canvas_shape_at (aswb->parent->canvas, aswb->swallowed->current, aswb->folder_x, aswb->folder_y, False );
            }
#endif
        }
    }else if( obj->magic == MAGIC_WHARF_FOLDER )
    {
        ASWharfFolder *aswf = (ASWharfFolder*)obj;
        ASFlagType changes = handle_canvas_config (aswf->canvas );
        if( aswf->animation_steps == 0 && get_flags( aswf->flags, ASW_Mapped ) && aswf->animation_dir < 0 )
        {
            unmap_wharf_folder( aswf );
        }else
        {
LOCAL_DEBUG_OUT("animation_steps = %d", aswf->animation_steps );
            if( aswf->animation_steps > 0 )
            {
                int new_width = 1, new_height = 1;
                animate_wharf( aswf, &new_width, &new_height );
                if( new_width == 0 || new_height == 0 ||
                    (new_width == aswf->canvas->width && new_height == aswf->canvas->height ))
                    unmap_wharf_folder( aswf );
                else
                {
                    LOCAL_DEBUG_OUT( "resizing folder from %dx%d to %dx%d", aswf->canvas->width, aswf->canvas->height, new_width, new_height );
                    resize_canvas( aswf->canvas, new_width, new_height) ;
                    ASSync( False ) ;
                    if( get_flags( Config->set_flags, WHARF_ANIMATE_DELAY ) && Config->animate_delay > 0 )
                        sleep_a_little( Config->animate_delay*10000 );
                    else
                        sleep_a_little( 10000 );
                }
            }else if( changes != 0 )
            {
                int i = aswf->buttons_num ;
                while( --i >= 0 )
                    on_wharf_button_moveresize( &(aswf->buttons[i]), event );
            }
            if( get_flags( changes, CANVAS_RESIZED ) )
                update_wharf_folder_shape( aswf );
        }
    }
}

void
press_wharf_button( ASWharfButton *aswb, int state )
{
    if( WharfState.pressed_button &&
        WharfState.pressed_button != aswb )
    {
        set_astbar_pressed( WharfState.pressed_button->bar, WharfState.pressed_button->canvas, False );
        WharfState.pressed_button = NULL ;
    }
    if( aswb &&
        WharfState.pressed_button != aswb )
    {
        set_astbar_pressed( aswb->bar, aswb->canvas, True );
        WharfState.pressed_state = state ;
        WharfState.pressed_button = aswb ;
    }
}

void
release_pressure()
{
    ASWharfButton *pressed = WharfState.pressed_button ;
LOCAL_DEBUG_OUT( "pressed button is %p", pressed );
    if( pressed )
    {
        if( pressed->folder )
        {
LOCAL_DEBUG_OUT( "pressed button has folder %p (%s)", pressed->folder, get_flags( pressed->folder->flags, ASW_Mapped )?"Mapped":"Unmapped" );
            if( get_flags( pressed->folder->flags, ASW_Mapped ) )
                withdraw_wharf_folder( pressed->folder );
            else
                display_wharf_folder( pressed->folder, pressed->canvas->root_x, pressed->canvas->root_y,
                                                       pressed->canvas->root_x+pressed->canvas->width,
                                                       pressed->canvas->root_y+pressed->canvas->height  );
        }else if( pressed->fdata )
        {
            print_func_data(__FILE__, __FUNCTION__, __LINE__, pressed->fdata);
            if( !get_flags( pressed->flags, ASW_SwallowTarget ) || pressed->swallowed == NULL )
            {  /* send command to the AS-proper : */
                SendCommand( pressed->fdata, 0);
            }
        }
        set_astbar_pressed( pressed->bar, pressed->canvas, False );
        WharfState.pressed_button = NULL ;
    }
}

void
on_wharf_pressed( ASEvent *event )
{
    ASMagic *obj = fetch_object( event->w ) ;
    if( obj == NULL )
        return;
    if( obj->magic == MAGIC_WHARF_BUTTON )
    {
        ASWharfButton *aswb = (ASWharfButton*)obj;
        ASWharfFolder *aswf = aswb->parent;
        if( event->x.xbutton.button == Button3 && aswb->parent == WharfState.root_folder )
            if( (WITHDRAW_ON_EDGE(Config) && (&(aswf->buttons[0]) == aswb || &(aswf->buttons[aswf->buttons_num-1]) == aswb)) ||
                 WITHDRAW_ON_ANY(Config))
        {
            if( get_flags( WharfState.root_folder->flags, ASW_Withdrawn) )
                display_main_folder();
            else
            {
                int wx = 0, wy = 0, wwidth, wheight;
                int i = aswf->buttons_num ;
                if( Config->withdraw_style < WITHDRAW_ON_ANY_BUTTON_AND_SHOW )
                    aswb = &(aswf->buttons[0]);

                wwidth = aswb->desired_width ;
                wheight = aswb->desired_height ;
                if( get_flags( Config->geometry.flags, XNegative ) )
                    wx = Scr.MyDisplayWidth - wwidth ;
                if( get_flags( Config->geometry.flags, YNegative ) )
                    wy = Scr.MyDisplayHeight - wheight ;

                set_flags( aswf->flags, ASW_Withdrawn );
                moveresize_canvas( aswf->canvas, wx, wy, wwidth, wheight );
                XRaiseWindow( dpy, aswb->canvas->w );
                while ( --i >= 0 )
                {
                    if( aswf->buttons[i].folder &&
                        get_flags( aswf->buttons[i].folder->flags, ASW_Mapped ) )
                        unmap_wharf_folder( aswf->buttons[i].folder );
                    if( &(aswf->buttons[i]) != aswb )
                    {
                        aswf->buttons[i].folder_x = wwidth ;
                        aswf->buttons[i].folder_y = wheight ;
                        move_canvas( aswf->buttons[i].canvas, wwidth, wheight );
                    }
                }
                aswb->folder_x = 0;
                aswb->folder_y = 0;
                aswb->folder_width = wwidth ;
                aswb->folder_height = wheight ;
                moveresize_canvas( aswb->canvas, 0, 0, wwidth, wheight );
            }
            return;
        }
        press_wharf_button( aswb, event->x.xbutton.state );
    }
}


