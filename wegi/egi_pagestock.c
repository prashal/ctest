/*-------------------------------------------------------------------------
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

page creation jobs:
1. egi_create_XXXpage() function.
   1.1 creating eboxes and page.
   1.2 assign thread-runner to the page.
   1.3 assign routine to the page.
   1.4 assign button functions to corresponding eboxes in page.
2. thread-runner functions.
3. egi_XXX_routine() function if not use default egi_page_routine().
4. button reaction functins

Midas Zhou
---------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "egi.h"
#include "egi_color.h"
#include "egi_txt.h"
#include "egi_objtxt.h"
#include "egi_btn.h"
#include "egi_page.h"
#include "egi_symbol.h"
#include "egi_log.h"
#include "egi_timer.h"
#include "egi_appstock.h"
#include "egi_pagestock.h"

/* icon code for button symbols */
#define ICON_CODE_PREV 		12
#define ICON_CODE_PAUSE 	13
#define ICON_CODE_PLAY 		15
#define ICON_CODE_NEXT 		14
#define ICON_CODE_EXIT 		16
#define ICON_CODE_SHUFFLE	17	/* pick next file randomly */
#define ICON_CODE_REPEATONE	18	/* repeat current file */
#define ICON_CODE_LOOPALL	19	/* loop all files in the list */

static uint16_t btn_symcolor;

static int stock_prev(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data);
static int stock_playpause(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data);
static int stock_next(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data);
static int stock_playmode(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data);
static int stock_exit(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data);


/*---------- [  PAGE ::  Mplayer Operation ] ---------
1. create eboxes for 4 buttons and 1 title bar

Return
	pointer to a page	OK
	NULL			fail
------------------------------------------------------*/
EGI_PAGE *egi_create_pagestock(void)
{
	int i;
	int btnum=5;
	EGI_EBOX *stock_btns[5];
	EGI_DATA_BTN *data_btns[5];

	/* --------- 1. create buttons --------- */
        for(i=0;i<btnum;i++) /* row of buttons*/
        {
		/* 1. create new data_btns */
		data_btns[i]=egi_btndata_new(i, /* int id */
						square, /* enum egi_btn_type shape */
						&sympg_buttons, /* struct symbol_page *icon. If NULL, use geometry. */
						0, /* int icon_code, assign later.. */
						&sympg_testfont /* for ebox->tag font */
						);
		/* if fail, try again ... */
		if(data_btns[i]==NULL)
		{
			EGI_PLOG(LOGLV_ERROR,"%s: fail to call egi_btndata_new() for data_btns[%d]. retry...\n", 
											__func__, i);
			i--;
			continue;
		}

		/* Do not show tag on the button */
		data_btns[i]->showtag=false;

		/* 2. create new btn eboxes */
		stock_btns[i]=egi_btnbox_new(NULL, /* put tag later */
						data_btns[i], /* EGI_DATA_BTN *egi_data */
				        	1, /* bool movable */
					        48*i, 320-(60-5), /* int x0, int y0 */
						48,60, /* int width, int height */
				       		0, /* int frame,<0 no frame */
		       				egi_color_random(medium) /*int prmcolor, for geom button only. */
					   );
		/* if fail, try again ... */
		if(stock_btns[i]==NULL)
		{
			printf("%s: fail to call egi_btnbox_new() for stock_btns[%d]. retry...\n", i, __func__);
			free(data_btns[i]);
			data_btns[i]=NULL;
			i--;
			continue;
		}
	}

	/* get a random color for the icon */
	btn_symcolor=egi_color_random(medium);
	EGI_PLOG(LOGLV_INFO,"%s: set 24bits btn_symcolor as 0x%06X \n",	__FUNCTION__, COLOR_16TO24BITS(btn_symcolor) );

	/* add tags,set icon_code and reaction function here */
	egi_ebox_settag(stock_btns[0], "Prev");
	data_btns[0]->icon_code=(btn_symcolor<<16)+ICON_CODE_PREV; /* SUB_COLOR+CODE */
	stock_btns[0]->reaction=stock_prev;

	egi_ebox_settag(stock_btns[1], "Play&Pause");
	data_btns[1]->icon_code=(btn_symcolor<<16)+ICON_CODE_PLAY; /* 13--pause, 15--play */
	stock_btns[1]->reaction=stock_playpause;

	egi_ebox_settag(stock_btns[2], "Next");
	data_btns[2]->icon_code=(btn_symcolor<<16)+ICON_CODE_NEXT;
	stock_btns[2]->reaction=stock_next;

	egi_ebox_settag(stock_btns[3], "Exit");
	data_btns[3]->icon_code=(btn_symcolor<<16)+ICON_CODE_EXIT;
	stock_btns[3]->reaction=stock_exit;

	egi_ebox_settag(stock_btns[4], "Playmode");
	data_btns[4]->icon_code=(btn_symcolor<<16)+ICON_CODE_SHUFFLE;
	stock_btns[4]->reaction=stock_playmode;



	/* --------- 2. create title bar --------- */
	EGI_EBOX *title_bar= create_ebox_titlebar(
	        0, 0, /* int x0, int y0 */
        	0, 2,  /* int offx, int offy */
		WEGI_COLOR_BLACK, //egi_colorgray_random(medium), //light),  /* int16_t bkcolor */
    		NULL	/* char *title */
	);
	egi_txtbox_settitle(title_bar, "	Stock Market ");


	/* --------- 3. create ffplay page ------- */
	/* 3.1 create ffplay page */
	EGI_PAGE *page_stock=egi_page_new("page_stock");
	while(page_stock==NULL)
	{
		printf("%s: fail to call egi_page_new(), try again ...\n",__func__);
		page_stock=egi_page_new("page_stock");
		tm_delayms(10);
	}

	page_stock->ebox->prmcolor=WEGI_COLOR_BLACK;

        /* 3.2 put pthread runner */
        page_stock->runner[0]=egi_stockchart;

        /* 3.3 set default routine job */
        page_stock->routine=egi_page_routine; /* use default routine function */

        /* 3.4 set wallpaper */
        page_stock->fpath=NULL; //"/tmp/mplay.jpg";

	/* 3.5 add ebox to home page */
	for(i=0;i<btnum;i++) /* add buttons */
		egi_page_addlist(page_stock, stock_btns[i]);
	egi_page_addlist(page_stock, title_bar); /* add title bar */


	return page_stock;
}


/*-----------------  RUNNER 1 --------------------------

-------------------------------------------------------*/
static void page_runner(EGI_PAGE *page)
{

}

/*--------------------------------------------------------------------
Stock PREV
return
----------------------------------------------------------------------*/
static int stock_prev(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data)
{
        /* bypass unwanted touch status */
        if(touch_data->status != pressing)
                return btnret_IDLE;

	/* only react to status 'pressing' */
	return btnret_OK;
}

/*--------------------------------------------------------------------
stock palypause
return
----------------------------------------------------------------------*/
static int stock_playpause(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data)
{
        /* bypass unwanted touch status */
        if(touch_data->status != pressing)
                return btnret_IDLE;

	/* only react to status 'pressing' */
	struct egi_data_btn *data_btn=(struct egi_data_btn *)(ebox->egi_data);

	/* toggle the icon between play and pause */
	if( (data_btn->icon_code<<16) == ICON_CODE_PLAY<<16 )
		data_btn->icon_code=(btn_symcolor<<16)+ICON_CODE_PAUSE;
	else
		data_btn->icon_code=(btn_symcolor<<16)+ICON_CODE_PLAY;

	/* set refresh flag for this ebox */
	egi_ebox_needrefresh(ebox);

	return btnret_OK;
}

/*--------------------------------------------------------------------
stock exit
return
----------------------------------------------------------------------*/
static int stock_next(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data)
{
        /* bypass unwanted touch status */
        if(touch_data->status != pressing)
                return btnret_IDLE;

	return btnret_OK;
}

/*--------------------------------------------------------------------
stock play mode
return
----------------------------------------------------------------------*/
static int stock_playmode(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data)
{
	static int count=0;

        /* bypass unwanted touch status */
        if(touch_data->status != pressing)
                return btnret_IDLE;

	/* only react to status 'pressing' */
	struct egi_data_btn *data_btn=(struct egi_data_btn *)(ebox->egi_data);

	count++;
	if(count>2)
		count=count>>3;

	/* rotate code: SHUFFLE -> REPEATONE -> LOOPALL -> */
	data_btn->icon_code=(btn_symcolor<<16)+(ICON_CODE_SHUFFLE+count%3);

	/* set refresh flag for this ebox */
	egi_ebox_needrefresh(ebox);

	return btnret_OK;
}

/*--------------------------------------------------------------------
stock exit
???? do NOT call long sleep function in button functions.
return
----------------------------------------------------------------------*/
static int stock_exit(EGI_EBOX * ebox, EGI_TOUCH_DATA * touch_data)
{
        /* bypass unwanted touch status */
        if(touch_data->status != pressing)
                return btnret_IDLE;

        egi_msgbox_create("Message:\n   Click! Start to exit stock page!", 300, WEGI_COLOR_ORANGE);
        return btnret_REQUEST_EXIT_PAGE;
}
