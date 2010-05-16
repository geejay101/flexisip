#include "agent.hh"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>

#define IPADDR_SIZE 32

static int get_local_ip_for_with_connect(int type, const char *dest, char *result){
	int err,tmp;
	struct addrinfo hints;
	struct addrinfo *res=NULL;
	struct sockaddr_storage addr;
	struct sockaddr *p_addr=(struct sockaddr*)&addr;
	int sock;
	socklen_t s;

	memset(&hints,0,sizeof(hints));
	hints.ai_family=(type==AF_INET6) ? PF_INET6 : PF_INET;
	hints.ai_socktype=SOCK_DGRAM;
	/*hints.ai_flags=AI_NUMERICHOST|AI_CANONNAME;*/
	err=getaddrinfo(dest,"5060",&hints,&res);
	if (err!=0){
		LOGE("getaddrinfo() error: %s",gai_strerror(err));
		return -1;
	}
	if (res==NULL){
		LOGE("bug: getaddrinfo returned nothing.");
		return -1;
	}
	sock=socket(res->ai_family,SOCK_DGRAM,0);
	tmp=1;
	err=setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&tmp,sizeof(int));
	if (err<0){
		LOGW("Error in setsockopt: %s",strerror(errno));
	}
	err=connect(sock,res->ai_addr,res->ai_addrlen);
	if (err<0) {
		LOGE("Error in connect: %s",strerror(errno));
 		freeaddrinfo(res);
 		close(sock);
		return -1;
	}
	freeaddrinfo(res);
	res=NULL;
	s=sizeof(addr);
	err=getsockname(sock,(struct sockaddr*)&addr,&s);
	if (err!=0) {
		LOGE("Error in getsockname: %s",strerror(errno));
		close(sock);
		return -1;
	}
	if (p_addr->sa_family==AF_INET){
		struct sockaddr_in *p_sin=(struct sockaddr_in*)p_addr;
		if (p_sin->sin_addr.s_addr==0){
			close(sock);
			return -1;
		}
	}
	err=getnameinfo((struct sockaddr *)&addr,s,result,IPADDR_SIZE,NULL,0,NI_NUMERICHOST);
	if (err!=0){
		LOGE("getnameinfo error: %s",strerror(errno));
	}
	close(sock);
	LOGI("Local interface to reach %s is %s.",dest,result);
	return 0;
}

int main(int argc, char *argv[]){
	Agent *a;
	int port=5060;
	char localip[IPADDR_SIZE];
	int i;

	for(i=1;i<argc;++i){
		if (strcmp(argv[i],"--port")==0){
			i++;
			if (i<argc){
				port=atoi(argv[i]);
			}
		}
	}

	
	su_init();
	su_root_t *root=su_root_create(NULL);

	get_local_ip_for_with_connect (AF_INET,"87.98.157.38",localip);
	
	a=new Agent(root,localip,port);
	su_root_run(root);

	delete a;
    su_root_destroy(root);
	
}

