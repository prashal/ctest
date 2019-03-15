/*------------------------------------------------------------------------------------------------
A EGI FIFO buffer

1. If overrun/underrun occurs, pin/pout reset to the same as pout/pin, a new rase
   starts then. In this way, difference between pin and pout always keeps within
   buff_size.

2. For overrun, we may take DIFFERENT stratigies:
   1. Overrun occurs when pin catch up pout from loopback, stop and wait pout to increase.
   2. Pin keeps increasing and old data in buff will be overwritten and lost, before pin
      gets them.

3. For underrun, pout wait for pin to increase. so pout alway lag behind pin.
   3.1 Case ahead=-1:
	Suppose ahead=0, and pin reachs item_size, while pout is being adjusted from item_size to 0,
   	in this case ahead--=-1, and underrun ocurrs.
   3.2 Other cases: pout==pin && ahead==0.

4. Keep same sequence rule for push and pull:
      1. First, check whether pin/pout == fifo->buff_size,  If so, reset pin/pout to 0.
      2. Then, check whether overrun or underrun.
      3. If let pushing go on even when overrun ocurrs, then after N times consecutive overrun,
	 total bytes of data loss may be N*(fifo->item_size) where N=1,2,3...

5. Fifo->ahead may be -1,0,1,2,in rare case 3...., heavier CPU load,bigger number.


Midas Zhou
---------------------------------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "egi_log.h"
#include "egi_debug.h"
#include "egi_utils.h"
#include "egi_fifo.h"


/*---------------------------------------------------------------
Malloc a EGI_FIFO structure and its buff.

efifo:		An empty pointer to EGI_FIFO.
buff_size: 	max data items to be hold in the buffer.
item_size:	data item size.

Return:
	!NULL	OK
	NULL	fails
---------------------------------------------------------------*/
EGI_FIFO * egi_malloc_fifo(int buff_size, int item_size)
{
	EGI_FIFO *efifo=NULL;

	/* check params */
	if(efifo != NULL)
		EGI_PLOG(LOGLV_WARN,"%s: input EGI_FIFO pointer is not NULL.\n",__func__);

	if( buff_size<=0 || item_size<=0 )
	{
		EGI_PLOG(LOGLV_ERROR,"%s: input buff_size or item_size is invalid.\n",
										__func__);
		return NULL;
	}


	/* malloc efifo */
	efifo=(EGI_FIFO *)malloc(sizeof(EGI_FIFO));
	if(efifo==NULL)
	{
		EGI_PLOG(LOGLV_ERROR,"%s: Fail to malloc efifo.\n",__func__);
		return NULL;
	}
	memset(efifo,0,sizeof(EGI_FIFO));

        /* buff mutex lock */
        if(pthread_mutex_init(&efifo->lock,NULL) != 0)
        {
                printf("%s: fail to initiate log_buff_mutex.\n",__func__);
		free(efifo);

                return NULL;
        }

	/* malloc buff, and memset inside... */
	efifo->buff=egi_malloc_buff2D(buff_size, item_size);
	if(efifo->buff==NULL)
	{
		EGI_PLOG(LOGLV_ERROR,"%s: Fail to malloc efifo->buff.\n", __func__);

		free(efifo);
		efifo=NULL;

		return NULL;
	}

	/* set EGI_FIFO member */
	efifo->buff_size=buff_size;
	efifo->item_size=item_size;
	efifo->pin  = 0;
	efifo->pout = 0;
	efifo->ahead=0;

	EGI_PLOG(LOGLV_INFO,"%s: An EGI_FIFO intialized successfully.\n", __func__);


	return efifo;
}


/*----------------------------------------------
Free a EGI_FIFO structure and its buff.
----------------------------------------------*/
void egi_free_fifo(EGI_FIFO *efifo )
{
	if( efifo != NULL)
	{
		/* free buff */
		if( efifo->buff != NULL )
		{
			printf("start egi_free_buff2D()...\n");
			egi_free_buff2D(efifo->buff, efifo->buff_size);
			//free(efifo->buff) and set efifo->buff=NULL in egi_free_buff2D() already
		}

		/* free itself */
		printf("%s:start free(efifo)...\n",__func__);
		free(efifo);
		efifo=NULL;
	}
}


/*--------------------------------------------------------------------
Push data into buff.
1. Keep difference between pin and pout within buff_size.
2. If overrun occurs, reset pin to the same vaule as pout and
   start a new chase, also reset ahead to 0.
3. Input data shall be same size of fifo->item_size, ????

fifo:	a EGI_FIFO holding the buff.
data:	data to push to fifo.
size:	size of the data to push to fifo, in byte.

Return:
	1	overrun;
	0	ok
	<0	fails
---------------------------------------------------------------------*/
int egi_push_fifo(EGI_FIFO *fifo,  unsigned char *data, int size)
{

	/* check input param */
	if( fifo==NULL || fifo->buff==NULL )
	{
		EGI_PDEBUG(DBG_FIFO,"Input fifo or fifo buff is NULL! \n");
		return -1;
	}
	if( data==NULL)
	{
		EGI_PDEBUG(DBG_FIFO,"Input data is NULL! \n");
		return -2;
	}
	if( size <= 0 || size > fifo->item_size )
	{
		EGI_PDEBUG(DBG_FIFO,"Data size exceeds FIFO buffer item_size, or size<=0.\n");
		return -2;
	}

////test
///	printf("fifo input data=%d\n",*(int *)data);


        /* get mutex lock */
        if(pthread_mutex_lock(&fifo->lock) != 0)
        {
                EGI_PLOG(LOGLV_CRITICAL,"%s: fail to get mutex lock.\n",__func__);
                return -3;
        }

 	/* >>>>>>>>>>>>>>>>>>>>   entering critical zone  >>>>>>>>>>>>>>>>>>>>> */

	/* Check if get end of the buff, loop back and increase 'ahead' */
	if( fifo->pin == fifo->buff_size )
	{
		fifo->pin=0;
		(fifo->ahead)++;

		/* this case shall never be expected */
		if( fifo->ahead > 1 || fifo->ahead < 0 )
			EGI_PLOG(LOGLV_ERROR,"fifo->ahead=%d \n",fifo->ahead);
	}

	/* Check if overrun occurs */

	/* NOTE: pin >= pout, NOT pin==put */
	if( (fifo->pin >= fifo->pout) && (fifo->ahead)>0 ) /* usually ahead=0 or 1 !!! */
	{
//		EGI_PLOG(LOGLV_WARN,"EGI_FIFO: !!! Input rate exceeds output, overrun occurrs!\n");

		/* reset fifo->pin to the save value of pout, start a new chase. */
		fifo->pin=fifo->pout;
		fifo->ahead=0;

		/* go on */
//		pthread_mutex_unlock(&fifo->lock);
//		return 1;
	}

	/* Now, fifo->pin is no more than buff_size-1. */

	/* Clear slot and push data */
	memset((fifo->buff)[fifo->pin],0,fifo->item_size);
	memcpy((fifo->buff)[fifo->pin],data,size); /* Not item_size however */

	/* increase pin at last */
	(fifo->pin) += 1;
	//printf("fifo->pin=%d\n",fifo->pin);

	pthread_mutex_unlock(&fifo->lock);

 	/* <<<<<<<<<<<<<<<<<<<<<<<<   leaving critical zone  <<<<<<<<<<<<<<<<<<<<<<<<  */


	return 0;
}


/*----------------------------------------------------------------------
Pull data from buff.
1. Keep difference between pin and pout within buff_size.
2. When underrun occurs, pout get the same positon as of pin,
   then it stops and waits pin to run ahead.

fifo:	a EGI_FIFO holding the buff.
data:	pointer to data pulled out from the fifo.
size:	size of the data to be extraced from the fifo, in byte.
	if size<=0 or >fifo->itemsize, then re_asign it to fifo->itemsize.

Return:
	1	underrun;
	0	ok
	<0	fails
-------------------------------------------------------------------------*/
int egi_pull_fifo(EGI_FIFO *fifo, unsigned char *data, int size)
{

	int item_size=size;

	/* check input param */
	if( fifo==NULL || fifo->buff==NULL )
	{
		EGI_PDEBUG(DBG_FIFO,"FIFO buff is NULL! \n");
		return -1;
	}
	/* check size */
	if( size <= 0 || size > fifo->item_size )
	{
		EGI_PDEBUG(DBG_FIFO,"Data size exceeds FIFO buffer item_size, or size<=0. reset to fifo->item_size.\n");
		item_size=fifo->item_size;
	}


        /* get mutex lock */
        if(pthread_mutex_lock(&fifo->lock) != 0)
        {
                EGI_PLOG(LOGLV_CRITICAL,"%s: fail to get mutex lock.\n",__func__);
                return -3;
        }

 	/* >>>>>>>>>>>>>>>>>>>>   entering critical zone  >>>>>>>>>>>>>>>>>>>>> */

	/* Check if get end of the buff, loop back and decrease 'ahead' */
	if( fifo->pout == fifo->buff_size )
	{
		fifo->pout=0;
		(fifo->ahead)--;

		/* this case shall never be expected */
		if(fifo->ahead > 1 || fifo->ahead < 0 )
			EGI_PLOG(LOGLV_ERROR,"fifo->ahead=%d \n",fifo->ahead);
	}

	/* Check if underrun occurs */
	if( ( (fifo->pout >= fifo->pin) && (fifo->ahead == 0) )  /* ahead: -1,0,1,2 */
		|| fifo->ahead < 0  ) /* when pin=item_size, and pout adjust from item_size to 0, surpass the start line, ahead-- =-1 */
	{
		EGI_PDEBUG(DBG_FIFO,"EGI_FIFO: !!! Output rate exceeds input, underrun occurrs!\n");

		/* return to wait pin */
		pthread_mutex_unlock(&fifo->lock);
		return 1;
	}

	/* Now, fifo->pout is no more than buff_size-1. */

	/* pull data */
	memcpy(data,(fifo->buff)[fifo->pout],item_size); /* Not fifo->item_size however */

	/* increase pin at last */
	(fifo->pout) += 1;
	//printf("fifo->pout=%d\n",fifo->pout);

	pthread_mutex_unlock(&fifo->lock);

 	/* <<<<<<<<<<<<<<<<<<<<<<<<   leaving critical zone  <<<<<<<<<<<<<<<<<<<<<<<<  */

	return 0;
}