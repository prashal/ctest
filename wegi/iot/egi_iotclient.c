/*--------------------------  iot_client.c  ---------------------------------------
1. A simple example to connect BIGIOT and control a bulb icon.
2. IoT talk syntax is according to BigIot Protocol: www.bigiot.net/help/1.html

3. More: iot_client works as an immediate layer between IoT server and EGI modules.
	3.1 One thread for sending and one for receiving.
	3.2 A recv_buff[] and a send_buff[].
	3.3 A parse thread, pull out recv_buff[], unwrap and forward to other modules.
	3.4 A wrap thread, wrap other modules's data to json string and push to send_buff[].
	what's more....

4. For one socket, there shall be only one recv_thread and one send_thread running at the same time.




			------	(( Glossary ))  ------
MTU:	    A maximum transmission unit(MTU) is the largest packet or frame size,IP uses MTU to determine
   	    the maximum size of each paket in the transmission. Max. 1500-bytes for internet.
	    A compelet packet:  Capsule(head and tail)+IP Header(20bytes)+TCP Header(20bytes)+Payload( )=Max.1500 bytes.

TCP MSS:    Maximum Segment Size is the payload of a TCP packet.
	    A complete TCP packet:  TCP Header(20bytes) + TCP MSS=Max.1480 bytes.
	    < ***** > IPv4 is required to handle Min MSS of 536 bytes, while IPv6 is 1220 bytes.


TODO:
1. recv() may return serveral sessions of BIGIOT command at one time, and fill log buffer full(254+1+1):
    //////  and cause log buff overflow !!!! \\\\\\\\
   [2019-03-12 10:21:52] [LOGLV_INFO] Message from the server: {"M":"say","ID":"Pc0a809a00a2900000b2b","NAME":"guest","C":"down","T":"1552357312"}
{"M":"say","ID":"Pc0a809a00a2900000b2b","NAME":"guest","C":"down","T":"1552357312"}
{"M":"say","ID":"Pc0a809a0[2019-03-12 10:22:03] [LOGLV_INFO] Message from the server: {"M":"say","ID":"Pc0a809a00a2900000b2b","NAME":"guest","C":"down","T":"1552357323"}
    ///// put recv() string to a buff......///

  ret==0:
	1). WiFi disconnects, network is down.    --- trigger iot_connect_checkin()

2. Calling recv() may return several IoT commands from socket buffer at one time, especailly in heavy load condition.
   so need to separate them.
3. check integrity of received message.
4. set TIMEOUT for recv() and send()
5. use select 

Midas Zhou
-------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <malloc.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <json.h>
#include <json_object.h>
#include <printbuf.h>
#include "egi_iotclient.h"
#include "egi.h"
#include "egi_debug.h"
#include "egi_log.h"
#include "egi_page.h"
#include "egi_color.h"
#include "egi_timer.h"
#include "egi_iwinfo.h"

#define IOT_SERVER_ADDR		"121.42.180.30" /* WWW.BIGIOT.NET */
#define IOT_SERVER_PORT 	8181
#define IOT_DEVICE_ID		"421"
#define IOT_DEVICE_KEY		"f80ea043e"

/* IOT data interface ID */
#define IOT_DATAINF_LOAD		546 /* CPU load */
#define IOT_DATAINF_WSPEED		961 /* network traffic, income */

#define IOT_HEARTBEAT_INTERVAL	30	/*in second, heart beat interval, Min 10s for status inquiry */
#define IOT_RECONNECT_INTERVAL	15	/*in second, reconnect interval, for status inquiry, Min. 10s  */

#define BUFFSIZE 		2048
#define BULB_OFF_COLOR 		0x000000 	/* default color for bulb turn_off */
#define BULB_ON_COLOR 		0xDDDDDD   	/* default color for bulb turn_on */
#define BULB_COLOR_STEP 	0x111111	/* step for turn up and turn down */
#define SUBCOLOR_UP_LIMIT	0xFFFFFF 	/* up limit value for bulb light color */
#define SUBCOLOR_DOWN_LIMIT	0x666666 	/* down limit value for bulb light color */


static int sockfd;
static struct sockaddr_in svr_addr;
static char recv_buf[BUFFSIZE]={0}; /* for recv() buff */

static EGI_24BIT_COLOR subcolor=0;
static bool bulb_off=false; /* default bulb status */
static char *bulb_status[2]={"ON","OFF"};
static bool keep_heart_beat=false; /* token for keepalive thread */

/*
  1. After receive command from the server, push it to incmd[] and confirm server for the receipt.
  2. After incmd[] executed, set flag incmd_executed to be true.
  3. Reply server to confirm result of execution.
*/
struct egi_iotdata
{
	//iot_status status;
	bool	connected;
	char 	incmd[64];
	bool	incmd_executed; /* set to false once after renew incmd[] */
};


/* json string templates as per BIGIOT protocol */
#define TEMPLATE_MARGIN		100 /* more bytes that may be added into the following json templates strings */
const static char *strjson_checkin_template=
			"{\"M\":\"checkin\",\"ID\":421, \"K\":\"f80ea043e\"}\n";
const static char *strjson_keepalive_template=
			"{\"M\":\"beat\"}\n";
const static char *strjson_reply_template=
			"{\"M\":\"say\",\"C\":\"Hello from Widora_NEO\",\"SIGN\":\"Widora_NEO\"}";//\n";
const static char *strjson_check_online_template=
			"{\"M\":\"isOL\",\"ID\":[\"xx1\"...]}\n";
const static char *strjson_check_status_template=
			"{\"M\":\"status\"}\n"; /* return key M value:'connected' or 'checked' */
const static char *strjson_update_template=
			"{\"M\":\"update\",\"ID\":\"421\"}";//to insert: \"V\":{\"id1\":\"value1\"}}\n";



/* Functions declaration */
const static char *iot_getkey_pstrval(json_object *json, const char* key);
static json_object * iot_new_datajson(const int *id, const double *value, int num);
static void iot_keepalive(void);
static int iot_connect_checkin(void);
static inline int iot_send(int *send_ret, const char *strmsg);
static inline int iot_recv(int *recv_ret);
static inline int iot_status(void);


/*-----------------------------------------------------------------------
Get a pionter to a string type key value from an input json object.

json:	a json object.
key:	key name of a json object

return:
	a pointer to string	OK
	NULL			Fail
------------------------------------------------------------------------*/
const static char *iot_getkey_pstrval(json_object *json, const char* key)
{
	json_object *json_item=NULL;
	char *pstr;

	/* check input param */
	if( json==NULL || key==NULL)
	{
		EGI_PDEBUG(DBG_IOT,"input params invalid!\n");
		return NULL;
	}

	/* get item object */
	//obselete:
	//json_item=json_object_object_get(json, key);
	if( json_object_object_get_ex(json, key, &json_item)==false ) /* return pointer */
	{
		//EGI_PDEBUG(DBG_IOT,"Fail to get value of key '%s' from the input json object.\n",key);
		return NULL;
	}
	else
	{
		//EGI_PDEBUG(DBG_IOT,"object key '%s' with value '%s'\n",
		//					key, json_object_get_string(json_item));
	}
	/* check json object type */
	if(!json_object_is_type(json_item, json_type_string))
	{
		EGI_PDEBUG(DBG_IOT,"Error, key '%s' json obj is NOT a string type object!\n",key );
		return NULL;
	}

	/* get a pointer to string value */
 	pstr=json_object_get_string(json_item); /* return a pointer */
	json_object_put(json_item);

	return pstr;
}

/*-----------------------------------------------------------------------
Assembly a cjson object by adding 'num' groups of keys(IDs) and Values,
result json obj is like {"id1":"value1","id2","value2",.... }.

Note: call json_object_put() to free it after use.

Retrun:
	NULL 	fails
-----------------------------------------------------------------------*/
static json_object * iot_new_datajson(const int *id, const double *value, int num)
{
	int k;
	/* buff for ID string:
         * for int32, its Max decimal value has 11 digits, and int64 has 19 digits
         */
	char strid[32];
	char strval[32];

	json_object *json_item;

	/* get a new json_object_type obj */
	json_object *json=json_object_new_object();
	if(json==NULL)
		return NULL;

	/* insert Keys and Values into the json object */
	for(k=0; k<num; k++)
	{
		memset(strid,0,32);
		sprintf(strid,"%d",id[k]);
		memset(strval,0,32);
		sprintf(strval,"%f",value[k]);
		/* with strdup() inside */
		json_object_object_add(json, strid, json_object_new_string(strval));
	}
	printf("%s: json created: %s\n",__func__,json_object_to_json_string(json));

	return json;
}


/*--------------------  A thread function   --------------------------

1. Send status inquiry string to keep connection with server.

Note:
   The function will not receive server replay message, just leave it
   to other threads to confirm.


Interval Min. 10 seconds as per protocol

-------------------------------------------------------------------*/
static void iot_keepalive(void)
{
	int ret;
	time_t t;
	struct tm *tm;

	while(1)
	{
		/* idle time */
		//tm_delayms(IOT_HEARTBEAT_INTERVAL*1000);
		EGI_PLOG(LOGLV_WARN,"---------- start egi_sleep %d ------------\n", IOT_HEARTBEAT_INTERVAL);
		egi_sleep(1,IOT_HEARTBEAT_INTERVAL,0);
		EGI_PLOG(LOGLV_WARN,"---------- end egi_sleep()------------\n");

		/* check iot network status */
		ret=iot_send(&ret, strjson_check_status_template);
		if(ret==0)
		{
			EGI_PDEBUG(DBG_IOT,"heart_beat msg is sent out.\n");
		        /* get time stamp */
        		t=time(NULL);
        		tm=localtime(&t);
        		/* time stamp and msg */
        		EGI_PLOG(LOGLV_INFO,"[%d-%02d-%02d %02d:%02d:%02d] heart_beat msg is sent out.\n",
                                tm->tm_year+1900,tm->tm_mon+1,tm->tm_mday,tm->tm_hour, tm->tm_min,tm->tm_sec );
			/* !!! WARNING: __TIME__ macro is inaccurate, has a deviation of several seconds. !!! */
			///printf("[%s %s] %s: A heart_beat msg is sent to BIGIOT.\n",__DATE__, __TIME__, __func__);
		}
		else
			EGI_PDEBUG(DBG_IOT,"Fail to send out heart_beat msg.\n");
	}
}


/*--------------  A Thread Function  ------------------
Update BIGIOT interface data by sending json string

Return:
	0	OK
	<0	fails
---------------------------------------------------------------------*/
static void iot_update_data(void)
{
	/* ID and DATA for BIGIOT data interface */
	int id[2]={ IOT_DATAINF_LOAD, IOT_DATAINF_WSPEED};	/* interface ID */
	double data[2];

	int ws;	/* wifi speed bytes/s */
	json_object *json_data;
	char sendbuff[BUFFSIZE]={0}; /* for recv() buff */
	int ret;
	json_object *json_update;

	/* open /porc/loadavg to get load value */
        double load=0.0;
        int fd;
        char strload[5]={0}; /* read in 4 byte */

        /* open symbol image file */
        fd=open("/proc/loadavg", O_RDONLY);
        if(fd<0)
        {
                EGI_PLOG(LOGLV_ERROR,"%s: fail to open /proc/loadavg!\n",__func__);
                pthread_exit(0);
        }

	/* prepare update template */
	/* WARNING: for an object_type json object containing another object_type json object,
	 *  when you call json_object_object_add() 2nd time, it incurrs segmentation fault!
	 *  you need to put it and then reinitialize it.
	 *
	 */
#if 0
	json_object *json_update=json_tokener_parse(strjson_update_template);
	if(json_update == NULL) {
		EGI_PLOG(LOGLV_ERROR,"egi_iotclient: fail to prepare json_update.\n");
		pthread_exit(0);
	}
#endif



	while(1)
	{
		/* prepare template */
		json_update=json_tokener_parse(strjson_update_template);

		/* read avgload */
                lseek(fd,0,SEEK_SET);
                read(fd,strload,4);
                load=atof(strload);/* for symmic_cpuload[], index from 0 to 5 */
		/* get wifi actual speed  bytes/s */
		iw_get_speed(&ws);
		printf("--------------   get wifi speed: %d(bytes/s)   -----------\n",ws);

		/* prepare data json */
		data[0]=load;
		data[1]=ws/IW_TRAFFIC_SAMPLE_SEC; /* get average value */

		printf("start call iot_new_datajson()....\n");
 		json_data=iot_new_datajson( (const int *)id, data, 2);
		if(json_data==NULL)
		{
		    /* free json_update then */
		    if(json_object_put(json_update) != 1)
			EGI_PLOG(LOGLV_ERROR,"%s: iot_new_datajson() fails, and Fail to put json_update!\n",__func__);
			continue;
		}

		/* insert data json to update template json */
		printf("start object_del()....\n");
		json_object_object_del(json_update,"V");
		printf("aft. V deleted: %s\n",json_object_to_json_string(json_update));
		printf("before start object_add():\n");
		json_object_object_add(json_update,"V",json_data);
		EGI_PLOG(LOGLV_CRITICAL,"Finish creating update_json: \n%s\n",
					json_object_to_json_string_ext(json_update,JSON_C_TO_STRING_PRETTY)); /* ret pointer only */
		/* add tail '\n' */
		printf("before memset sendbuff...\n");
		memset(sendbuff,0,sizeof(sendbuff));
		printf("before sprintf sendbuff...\n");
		sprintf(sendbuff,"%s\n",json_object_to_json_string(json_update));

		/* send to iot */
		printf("before iot_send()...\n");
		if (iot_send(&ret, sendbuff) != 0)
			EGI_PLOG(LOGLV_WARN,"%s: fail to iot_send() updata message, ret=%d.\n",
											__func__, ret);

		/* release json_data */
		if(json_object_put(json_data) != 1)
			EGI_PLOG(LOGLV_ERROR,"Fail to json_object_put() json_data!\n");
		if(json_object_put(json_update) != 1)
			EGI_PLOG(LOGLV_ERROR,"Fail to json_object_put() json_update!\n");

		/* delay and refresh value */
		//tm_delayms(6000);
		egi_sleep(2,6,0);
	}

	close(fd);
}



/*--------------------------------------------------------
1. Close and set up socket FD
2. Connect to IoT server
3. Check in.

NOTE: Function loops trying to check into BigIot untill it
      succeeds.

Return:
	0	OK
	<O	fails
---------------------------------------------------------*/
static int iot_connect_checkin(void)
{
	int ret;
	//char buf[BUFFSIZE]={0}; /* for recv() buff */

   /* loop trying .... */

   for(;;)
   {
	EGI_PLOG(LOGLV_CRITICAL,"------ Start connect and check into ther server ------\n");

	/* create a socket file descriptor */
	close(sockfd); /*close it  first */
	sockfd=socket(AF_INET,SOCK_STREAM,0);
	if(sockfd<0)
	{
		perror("try to create socket file descriptor");
		EGI_PLOG(LOGLV_ERROR,"%s :: socket(): %s \n",__func__,strerror(errno));

		continue; //return -1;
	}
	//printf("Succeed to create a socket file descriptor!\n");

	/* set IOT server address  */
	svr_addr.sin_family=AF_INET;
	svr_addr.sin_port=htons(IOT_SERVER_PORT);
	svr_addr.sin_addr.s_addr=inet_addr(IOT_SERVER_ADDR);
	bzero(&(svr_addr.sin_zero),8);

   	/* connect to socket. */
	ret=connect(sockfd,(struct sockaddr *)&svr_addr, sizeof(struct sockaddr));
	if(ret<0)
	{
		EGI_PLOG(LOGLV_ERROR,"%s :: connect(): %s \n",__func__,strerror(errno));
		continue;//return -2;
	}

	EGI_PLOG(LOGLV_CRITICAL,"%s: Succeed to connect to the BIGIOT socket!\n",__func__);

	memset(recv_buf,0,sizeof(recv_buf));
	ret=recv(sockfd, recv_buf, BUFFSIZE-1, 0);
	if(ret<=0)
	{
		EGI_PLOG(LOGLV_ERROR,"%s :: recv(): %s \n",__func__,strerror(errno));
		continue;
	}

	//recv_buf[ret]='\0';
	EGI_PLOG(LOGLV_CRITICAL,"Reply from the server: %s\n",recv_buf);

	/* send json string for check into BIGIO */
	EGI_PLOG(LOGLV_CRITICAL,"Start sending CheckIn msg to BIGIOT...\n");
	ret=send(sockfd, strjson_checkin_template, strlen(strjson_checkin_template), MSG_NOSIGNAL);
	if(ret<=0)
	{
		EGI_PLOG(LOGLV_ERROR,"%s: Fail to send login request to BIGIOT:%s\n"
								,__func__,strerror(errno));
		continue;//return -3;
	}
	else
		EGI_PLOG(LOGLV_CRITICAL,"Checkin msg has been sent to BIGIOT.\n");

	/* wait for reply from the server */
	memset(recv_buf,0,sizeof(recv_buf));
	ret=recv(sockfd, recv_buf, BUFFSIZE-1, 0);
	if(ret<=0)
	{
		EGI_PLOG(LOGLV_ERROR,"%s: Fail to recv() CheckIn confirm msg from BIGIOT, %s\n"
								,__func__,strerror(errno));
		continue;//return -4;
	}
	else if( strstr(recv_buf,"checkinok") ==NULL ) {
		EGI_PLOG(LOGLV_ERROR,"Checkin confirm msg is NOT received. retry after INTERVAL seconds...\n");
	        tm_delayms(IOT_RECONNECT_INTERVAL*1000);

		continue;//return -5;
	}
	else {
		EGI_PLOG(LOGLV_CRITICAL,"EGI_IOT: Checkin confirm msg is received.\n");
	}

	EGI_PLOG(LOGLV_CRITICAL,"CheckIn reply from the server: %s\n",recv_buf);

	/* finally break the loop */
	break;

    }/* loop end */

	return 0;
}



/*--------------------------------------------------------------
1. send msg to IoT server by a BLOCKING socket FD.

Param:
	send_ret:  pointer to return value of send() call.
	strmsg:	   pointer to a message string.

Return:
	0	OK.
	<0	send() fails.
--------------------------------------------------------------*/
static inline int iot_send(int *send_ret, const char *strmsg)
{
	int ret;

	/* check strmsg */
	if( strlen(strmsg)==0 || strmsg==NULL )
		return -1;

	/* send by a blocking socke FD, */
	ret=send(sockfd, strmsg, strlen(strmsg), MSG_CONFIRM|MSG_NOSIGNAL);

		/* pass ret to the caller */
	if(send_ret != NULL)
		*send_ret=ret;

	/* check result */
	if(ret<0)
	{
		EGI_PDEBUG(DBG_IOT,"Call send() error, %s\n", strerror(errno));
		return -2;
	}
	else
		EGI_PDEBUG(DBG_IOT,"Succeed to send msg to the server.\n");

	return 0;
}

/*-----------------------------------------------------------------
1. Receive msg from IoT server by a BLOCKING socket FD.
2. Check data integrity then by verifying its head and end token.
3. Received string are stored in recv_buf[].

Param:
	recv_ret:  pointer to return value of recv() call.

Return:
	0	OK
	<0	recv() fails
	>0	invalid IoT message received
------------------------------------------------------------------*/
static inline int iot_recv(int *recv_ret)
{
	int ret;
	int len;

        /* clear buff */
        memset(recv_buf,0,sizeof(recv_buf));

	/* receive with BLOCKING socket */
        ret=recv(sockfd, recv_buf, BUFFSIZE-1, 0);

	/* pass ret */
	if(recv_ret != NULL)
		*recv_ret=ret;

	/* return error */
        if(ret<=0)
                return -1;

	/* check integrity :: by head and end token */
	len=strlen(recv_buf);
	if( ( recv_buf[0] != '{' ) || (recv_buf[len-1] != '\n') )
	{
		EGI_PLOG(LOGLV_ERROR,"%s: ********* Invalid BigIoT message received: %s ******** \n",__func__,recv_buf);
		return 1;
	}

	return 0;
}

/*-------------------------------------------------------
Check IoT network status

Return:
	2	received data invalid
	1	Connected, but not checkin.
	0	OK, checkin.
	-1	Fail to call send()
	-2	Fail to call recv()
-------------------------------------------------------*/
static inline int iot_status(void)
{
	int ret;

	if( iot_send(&ret, strjson_check_status_template) !=0 )
		return -1;

	if( iot_recv(&ret) !=0 )
		return -2;

	if(strcmp(recv_buf,"checked")==0)
		return 0;

	else if(strcmp(recv_buf,"connected")==0)
		return 1;

	else  /*received invalid data */
		return 2;
}


/*----------------  Page Runner Function  ---------------------
Note: runner's host page may exit, so check *page to get status
BIGIOT client
--------------------------------------------------------------*/
void egi_iotclient(EGI_PAGE *page)
{
	int ret;
	int jsret;

	json_object *json=NULL; /* sock received json */
	json_object *json_reply=NULL; /* json for reply */
	json_object *json_item=NULL;

	struct printbuf*   pjbuf=printbuf_new(); /* for json string */

	char  keyC_buf[128]={0}; /* for key 'C' reply string*/

	char *strreply=NULL;
	const char *pstrIDval=NULL; /* key ID string pointer */
	const char *pstrCval=NULL; /* key C string pointer*/
	const char *pstrtmp=NULL;

	pthread_t  	pthd_keepalive;
	pthread_t	pthd_update; /* update BIGIOT interface data */

 	EGI_PDEBUG(DBG_PAGE,"page '%s': runner thread egi_iotclient() is activated!.\n"
                                                                                ,page->ebox->tag);

	/* magic operation */
//	mallopt(M_MMAP_THRESHOLD,0);


	/* get related ebox form the page, id number for the IoT button */
	EGI_EBOX *iotbtn=egi_page_pickebox(page, type_btn, 7);
	if(iotbtn == NULL)
	{
		EGI_PLOG(LOGLV_ERROR,"%s: Fail to pick the IoT button in page '%s'\n.",
								__func__,  page->ebox->tag);
		return;
	}
	egi_btnbox_setsubcolor(iotbtn, COLOR_24TO16BITS(BULB_ON_COLOR)); /* set subcolor */
	egi_ebox_needrefresh(iotbtn);
	//egi_page_flag_needrefresh(page); /* !!!SLOW!!!, put flag, let page routine to do the refresh job */
	egi_ebox_refresh(iotbtn);
	//printf("egi_iotclient: got iot button '%s' and reset subcolor.\n", iotbtn->tag);

	/* 	1. prepare jsons with template string */
#if 0
	json_reply=json_tokener_parse(strjson_reply_template);
	if(json_reply == NULL) {
		EGI_PLOG(LOGLV_ERROR,"egi_iotclient: fail to prepare json_reply.\n");
		return;
	}
#endif

	//printf("prepare json_reply as: %s\n",json_object_to_json_string(json_reply));

	/* 	2. Set up socket, connect and check into BigIot.net */
	iot_connect_checkin();

	/* 	3.1 Launch keepalive thread, BIGIOT */
#if 1
	if( pthread_create(&pthd_keepalive, NULL, (void *)iot_keepalive, NULL) !=0 ) {
                EGI_PLOG(LOGLV_ERROR,"Fail to create iot_keepalive thread!\n");
                goto fail;
        }
	else
		EGI_PDEBUG(DBG_IOT,"Create bigiot_keepavlie thread successfully!\n");
#endif
	/* 	3.2 Launch keepalive thread, BIGIOT */
#if 1
	if( pthread_create(&pthd_update, NULL, (void *)iot_update_data, NULL) !=0 ) {
                EGI_PLOG(LOGLV_ERROR,"Fail to create iot_update_data thread!\n");
                goto fail;
        }
	else
		EGI_PDEBUG(DBG_IOT,"Create iot_update_data thread successfully!\n");
#endif

	/* 	4. ------------  IoT Talk Loop:  loop recv and send processing  ---------- */
	while(1)
	{
		if( iot_recv(&ret)==0 )
		{
			printf("Message from the server: %s\n",recv_buf);
			EGI_PLOG(LOGLV_INFO,"Message from the server: %s\n",recv_buf);


			/* 4.1 parse received string for json */
			json=json_tokener_parse(recv_buf);/* _parse() creates a new json obj */
			if(!json)
			{
				EGI_PLOG(LOGLV_WARN,"egi_iotclient: Fail to parse received string by json_tokener_parse()!\n");
			}
			else /* extract key items and values */
			{
/* ---- key 'ID' ---- */
				/* 4.2 get a key value from a string object, ret a pointer only. */
				pstrIDval=iot_getkey_pstrval(json, "ID");
				/* Need to check pstrIDvall ????? */
				if(pstrIDval==NULL)
				 {
				  	 EGI_PDEBUG(DBG_IOT, "key 'ID' not found, continue...\n");
				   	 continue;
				 }
				/* 4.3 renew reply_json's ID value: with visitor's ID value
				 * delete old "ID" key, then add own ID for reply
				 */
				json_reply=json_tokener_parse(strjson_reply_template);

				json_object_object_del(json_reply, "ID");
				json_item=json_object_new_string(pstrIDval);
				json_object_object_add(json_reply, "ID", json_item); /* with strdup() inside */
				json_object_put(json_item);
				/* NOTE: json_object_new_string() with strdup() inside */

/* ---- key 'C' ---- */
				/* 4.4 get a pointer to key 'C'(command) string value, and parse the string  */
				pstrCval=iot_getkey_pstrval(json, "C");/* get visiotr's Command value */
				if(pstrCval != NULL)
				{
					EGI_PDEBUG(DBG_IOT,"receive command: %s\n",pstrCval);
					/* parse command string */
					if(strcmp(pstrCval,"offOn")==0)
					{
						EGI_PDEBUG(DBG_IOT,"Execute command 'offOn' ....\n");
						/* toggle the bulb color */
						bulb_off=!bulb_off;
						if(bulb_off) {
							EGI_PDEBUG(DBG_IOT,"Switch bulb OFF \n");
							subcolor=BULB_OFF_COLOR;
						}
						else {
							EGI_PDEBUG(DBG_IOT,"Switch bulb ON \n");
							subcolor=BULB_ON_COLOR; /* mild white */
						}

					}
					/* parse command 'plus' and 'up' */
					if( !bulb_off && (  (strcmp(pstrCval,"plus")==0)
							    || (strcmp(pstrCval,"up")==0)  ) )
					{
  					    	EGI_PDEBUG(DBG_IOT,"Execute command 'plus/up' ....\n");
						/* up limit value */
					    	if(  subcolor <  SUBCOLOR_UP_LIMIT ) {
							subcolor += BULB_COLOR_STEP;
					    	}
					}
					/* parse command 'minus' and 'down' */
					else if( !bulb_off && (  (strcmp(pstrCval,"minus")==0)
						         ||(strcmp(pstrCval,"down")==0)  ) )
					{
				    		EGI_PDEBUG(DBG_IOT,"Execute command 'minus/down' ....\n");
				    		if( subcolor < SUBCOLOR_DOWN_LIMIT ) {
							subcolor=SUBCOLOR_DOWN_LIMIT; /* low limit */
						}
				    		else {
							subcolor -= BULB_COLOR_STEP;
				    		}
					}

					//printf("subcolor=0x%08X \n",subcolor);
					/* set subcolor to iotbtn */
					egi_btnbox_setsubcolor(iotbtn, COLOR_24TO16BITS(subcolor)); //iotbtn_subcolor);
					/* refresh iotbtn */
					egi_ebox_needrefresh(iotbtn);
					//egi_page_flag_needrefresh(page); /* !!!SLOW!!!! let page routine to do the refresh job */
					egi_ebox_refresh(iotbtn);
				} /* end of (pstrCval != NULL) */

				/* 4.4 renew reply_json key 'C' value: with message string for reply */
				memset(keyC_buf,0,sizeof(keyC_buf));
				sprintf(keyC_buf,"'%s', bulb: %s, light: 0x%06X",
								pstrCval, bulb_status[bulb_off], subcolor);
				json_object_object_del(json_reply, "C");
				json_item=json_object_new_string(keyC_buf);
				json_object_object_add(json_reply,"C", json_item);
				json_object_put(json_item);
				/* 4.5 prepare reply string for socket */
				pstrtmp=json_object_to_json_string(json_reply);/* json..() return a pointer */
				strreply=(char *)malloc(strlen(pstrtmp)+TEMPLATE_MARGIN+2); /* at least +2 for '/n/0' */
				if(strreply==NULL)
				{
					EGI_PLOG(LOGLV_ERROR,"%s: fail to malloc strreply.\n",__func__);
					goto fail;
				}
				memset(strreply,0,strlen(pstrtmp)+TEMPLATE_MARGIN+2);
				//printf("sprintf strreply from json_object_to_json_string(json_reply)...\n");
				sprintf(strreply, "%s\n",
						json_object_to_json_string_ext(json_reply,JSON_C_TO_STRING_PRETTY) );/* json().. retrun a pointer */
				EGI_PDEBUG(DBG_IOT,"reply json string: %s\n",strreply);

				/* 4.6 send reply string by sockfd, to reply the visitor */
				ret=send(sockfd, strreply, strlen(strreply), MSG_CONFIRM|MSG_NOSIGNAL);
				if(ret<0)
				{
					EGI_PLOG(LOGLV_ERROR,"%s: send() error, %s\n",__func__, strerror(errno));
				}
				EGI_PDEBUG(DBG_IOT,"Reply to visitor successfully with: %s\n",strreply);

			} /* end of else (json) < parse msg > */

			/* clear arena for next IoT talk */
			/* defre and free json objects */
			if(strreply !=NULL )
				free(strreply);

			json_object_put(json_reply);

			if( (jsret=json_object_put(json_item)) !=1)
				EGI_PLOG(LOGLV_ERROR,"%s: Fail to put(free) json_item!, jsret=%d \n",
											__func__, jsret);

			if( (jsret=json_object_put(json)) != 1) /* func with if(json) inside  */
				EGI_PLOG(LOGLV_ERROR,"%s: Fail to put(free) json!, jsret=%d \n",
											__func__, jsret);

		} /* end of if(iot_recv(&ret)==0) */
		else if(ret==0) /* socket disconnected, reconnect again. */
		{
			EGI_PLOG(LOGLV_ERROR,"%s: recv() ret=0 \n",__func__ );
			/* trap into loop of connect&checkin untill it succeeds. */
			iot_connect_checkin();
		}
		else /* ret<0 */
		{
			EGI_PLOG(LOGLV_ERROR,"%s: recv() error, %s\n",__func__, strerror(errno));
			if(ret==EBADF) /* invalid sock FD */
				EGI_PLOG(LOGLV_ERROR,"%s: Invalid socket file descriptor!\n",__func__);
			if(ret==ENOTSOCK) /* invalid sock FD */
				EGI_PLOG(LOGLV_ERROR,"%s: The file descriptor sockfd does not refer to a socket!\n",__func__);
		}

		//tm_delayms(IOT_CLIENT_);
	} /* end of while() */

	/* joint threads */
	pthread_join(pthd_keepalive,NULL);
	pthread_join(pthd_update,NULL);

fail:
	/* free var */
	if(strreply != NULL )
		free(strreply);
	/* release json object */
	json_object_put(json); /* func with if(json) inside */
	//// put other json objs //////

	/* free printbuf */
	printbuf_free(pjbuf);

	/* close socket FD */
	close(sockfd);

	return ;
}
