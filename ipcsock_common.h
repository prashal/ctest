/*------------------------------------------------------------
               --- BUGs and TODOs ---
1. detect adn delete disconnected IPCSock client from struct_SockClients[]
2. mutex lock for shared dat
--------------------------------------------------------------*/
#ifndef __IPCSOCK_COMMON_H__
#define __IPCSOCK_COMMON_H__

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h> //unix std socket
#include <sys/select.h>


#define  IPC_SOCK_PATH "/tmp/ipc_socket"

#define MAX_IPCSOCK_CLIENTS 5 //Max. number of IPC socket clients

//----- define IPC MESSAGE CODE -----
#define IPCMSG_NONE 0 //msg cleared token, ignore this msg dat, no need to send or parse
#define IPCMSG_PWM_THRESHOLD 1  // 1 for pwm threshold
#define IPCMSG_MOTOR_DIRECTION 2 // dat=0 run, dat=1 reverse
#define IPCMSG_MOTOR_STATU 3  // dat=0 stop, dat=1 run


//----- Local IPC Sock Server,
struct sockaddr_un svr_unaddr;//UNIX type socket address
int svr_fd;//server IPC sock fd

//------Local IPC Sock Client Struct -----
struct struct_IPCSock_Client{
int sock_fd;	               //---- set 0 for no entry yet
struct sockaddr_un sock_unaddr; //UNIX addr.
//int unaddr_len;//len of sock_unaddr.
};
struct struct_IPCSock_Client  struct_SockClients[MAX_IPCSOCK_CLIENTS]={0};
int count_SockClients=0;
int max_fd=0; //for select()  =0: not client connected.

//------ FD SELECT -----
fd_set set_SockClients; //for READ Only 



//----- IPC Message Data struct -----
struct struct_msg_dat{
int msg_id; //--type of dat
int dat;
};

static struct struct_msg_dat msg_dat;


/*---------------------------------------------------------
1. create IPC server socket,
2. bind it with UNIX local addr(IPC_SOCK_PATH)
3. listen to the server sock.
4. accept client connections until MAX_IPCSOCK_CLIENTS.
5. put client fd to set_SockClients for select()
//5. loop in receiving and update msg_dat.
Note: Client disconnection case is not considered.
return <0 if fail.
-----------------------------------------------------------*/
static int create_IPCSock_Server(struct struct_msg_dat *pmsg_dat)
{
	struct sockaddr_un clt_unaddr;
	int i;
	int clt_fd;//client IPC sock fd
	int len;
	int nread;
	int ret;

	//------ reset msg_data
	memset(pmsg_dat,0,sizeof(struct struct_msg_dat));

	//----- 1. create ipc socket
	svr_fd = socket(PF_UNIX,SOCK_STREAM,0);
	if(svr_fd < 0){
		perror("Fail to create IPC server socket!");
		return -1;
	}

	//----- 2. specify ipc socket path name 
	svr_unaddr.sun_family=AF_UNIX; 
	strncpy(svr_unaddr.sun_path,IPC_SOCK_PATH,sizeof(svr_unaddr.sun_path)-1); // ??? why -1, end of cstring?
	unlink(IPC_SOCK_PATH); //if same name path exists, delete it.

	//----- 3. bind socket with addr.
	ret=bind(svr_fd,(struct sockaddr*)&svr_unaddr,sizeof(svr_unaddr));
	if(ret ==-1){
		perror("Fail to bind ipc socket with unix address");
		close(svr_fd);
		unlink(IPC_SOCK_PATH);
		return -2;
	}

	//----- 4. listen svr_fd
	ret=listen(svr_fd,MAX_IPCSOCK_CLIENTS);
	if(ret == -1){
		perror("Fail to listen to svr_fd");
		close(svr_fd);
		unlink(IPC_SOCK_PATH);
		return -3;
	}

	//----- 5. accept request connection store new clients to struct_SockClients[] ----
//	for(count_SockClients=0; count_SockClients<MAX_IPCSOCK_CLIENTS; count_SockClients++)
//	for(i=0; i<MAX_IPCSOCK_CLIENTS; i++)
	while(1)
	{
		len=sizeof(clt_unaddr);

		//--- wait for availble slot in struct_SockClients[] ----
		//count_SockClients is shared data and  will be modified by other threads
		if(count_SockClients >= MAX_IPCSOCK_CLIENTS){
			usleep(100000);
			continue;
		}

		//--accept clients in Blocking way
		clt_fd=accept(svr_fd,(struct sockaddr*)&clt_unaddr,&len);
		if(clt_fd <0 ){
			perror("Fail to accept connecting client requent!");
			close(svr_fd);
			//if(count_SockClients == 0)
			unlink(IPC_SOCK_PATH); // ????? unlink when fails accepting FIRST client ?????
			return -4;
		}
		printf("create_IPCSock_Server(): Sock Client fd=%d is accepted and added to struct_SockClients[%d] \n",clt_fd,count_SockClients);
		//---- find a free slot in struct_SockClients[]
		for(i=0; i<MAX_IPCSOCK_CLIENTS; i++){
			if(struct_SockClients[i].sock_fd == 0)
				break;
		}
		//---- store new clients to struct_SockClients[]
		struct_SockClients[i].sock_unaddr = clt_unaddr;
		struct_SockClients[i].sock_fd = clt_fd;
		//---- incread count for clients
		count_SockClients++;
		if(count_SockClients == MAX_IPCSOCK_CLIENTS)
			printf("reate_IPCSock_Server(): clients number reaches MAX_IPCSOCK_CLIENTS, end of function. \n");
		//--- get max_fd for select()
		if(clt_fd > max_fd)
			max_fd=clt_fd;

	}//end of while(;;)


}


/*---------------------------------------------------------
1. select to readable IPCSock Clients
2. update msg_dat  with  received data
-----------------------------------------------------------*/
static int read_IPCSock_Clients(struct struct_msg_dat *pmsg_dat)
{
	int i;
	int nclients;
	int nselect;
	int nread;

	while(1)
	{
		//---- if there is no client connected ----
		if(max_fd == 0){
			usleep(100000);
			continue;
		}

		//------ select() error will make its param. set_SockClients undfined, so 
		//you need to clear FD SET everytime becfore calling select()
		FD_ZERO(&set_SockClients);
		//---- add all accepted clients to FD_SET
		for(i=0;i<MAX_IPCSOCK_CLIENTS;i++){
			if(struct_SockClients[i].sock_fd != 0) //only if the entry is valid
				FD_SET(struct_SockClients[i].sock_fd, &set_SockClients);
		}
		//----- select only readable fd, by Blocking way ----
		nselect=select(max_fd+1,&set_SockClients,NULL,NULL,NULL);

		if(nselect < 0){
			perror("read_IPCSock_Clients(): select()");
		}
		else if (nselect > 0){

			//----- get readable Sock client -----
			for(i=0; i<MAX_IPCSOCK_CLIENTS; i++){
				if( struct_SockClients[i].sock_fd == 0) //skip unvalid entry
					continue;
				if(FD_ISSET( struct_SockClients[i].sock_fd, &set_SockClients )){
					printf("read_IPCSock_Clients(): Socket client fd=%d is selected. \n",struct_SockClients[i]);
					//---TODO: msg_dat before read.....

					//---- read msg_dat from IPC Sock Client ----
					nread = read(struct_SockClients[i].sock_fd,pmsg_dat,sizeof(struct struct_msg_dat));

					//----- check if it's from disconnected client  -----
					if(nselect==1 && nread==0){
						printf("read_IPCSock_Clients(): Socket client fd=%d is found disconnected!. \n",struct_SockClients[i]);
						struct_SockClients[i].sock_fd=0; //set 0 as empty the entry
						count_SockClients--; //decrease count
					}
					//------ read msg from client -----
					else if( nread>0 && nread != sizeof(struct struct_msg_dat)){
						printf("Received msg_dat is NOT complete!");
						pmsg_dat->msg_id = IPCMSG_NONE; // mark invalid msg_dat received.
					}
					else if (nread == sizeof(struct struct_msg_dat))
						printf("msg_dat from client msg_id: %d  dat: %d \n",pmsg_dat->msg_id,pmsg_dat->dat);

					//---TODO: unlock msg_dat after read.....

					break; //break for()

				}
			}//end for()

		}
	}//end while()
}


/*---------------------------------------------------------
1. create IPC client socket,
2. connect to the server sock.
3. connect to ipc sock server.
4. loop sending msg to sock server.
5. close sock fd
return <0 if fail
-----------------------------------------------------------*/
static int create_IPCSock_Client(struct struct_msg_dat *pmsg_dat)
{
        struct sockaddr_un svr_unaddr;
        int clt_fd;//server and connecting client FD
        int ret;
        int nwrite;

        //----- 1. create ipc socket
        clt_fd = socket(PF_UNIX,SOCK_STREAM,0);
        if(clt_fd < 0){
                perror("Fail to create IPC client socket!");
                return -1;
        }
	else
		printf("IPCSockClient: Succeed to create IPC Sock FD!\n");

        //----- 2. specify ipc socket path, which should be created by the server.
        svr_unaddr.sun_family=AF_UNIX; 
        strncpy(svr_unaddr.sun_path,IPC_SOCK_PATH,sizeof(svr_unaddr.sun_path)-1); // ??? why -1? end of cstring ??

        //----- 3. connect to ipc socket server
        ret=connect(clt_fd,(struct sockaddr*)&svr_unaddr,sizeof(svr_unaddr));
        if(ret == -1){
                perror("Fail to connect to ipc socket server");
                close(clt_fd);
                exit; //exit its main()
        }
	else
		printf("IPCSockClient: Succeed to connect to server!\n");

	//---- !!!!! reset msg_dat in application function -------

        //----- 4. loop send message to ipc socket server
	while(1){
		if(pmsg_dat->msg_id != IPCMSG_NONE){ //Only if msg_dat is valid.
        		nwrite=write(clt_fd,pmsg_dat,sizeof(struct struct_msg_dat));
			if(nwrite != sizeof(struct struct_msg_dat)){//if invalid msg_dat received
				printf("IPCSock_Client: nwrite=%d ,while size of strut_msg_dat is %d, NOT complete! \n",nwrite,sizeof(struct struct_msg_dat));

			}
			else{
				//-- set msg_id IPCMSG_NONE after finish nwrite
				pmsg_dat->msg_id = IPCMSG_NONE;
				printf("IPCSockClient: Succeed to write msg_dat to IPC Socket!\n");
			}
		}

		usleep(20000);

	}//while

        //----- 5. complete session
        close(clt_fd);


}

#endif
