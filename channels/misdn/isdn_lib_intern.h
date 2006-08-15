#ifndef ISDN_LIB_INTERN
#define ISDN_LIB_INTERN


#include <mISDNuser/mISDNlib.h>
#include <mISDNuser/isdn_net.h>
#include <mISDNuser/l3dss1.h>
#include <mISDNuser/net_l3.h>

#include <pthread.h>

#include "isdn_lib.h"


#if !defined MISDNUSER_VERSION_CODE || (MISDNUSER_VERSION_CODE < MISDNUSER_VERSION(1, 0, 3))
#error "You need a newer version of mISDNuser ..."
#endif


#define QI_ELEMENT(a) a.off


#ifndef mISDNUSER_HEAD_SIZE

#define mISDNUSER_HEAD_SIZE (sizeof(mISDNuser_head_t))
/*#define mISDNUSER_HEAD_SIZE (sizeof(mISDN_head_t))*/
#endif


ibuffer_t *astbuf;
ibuffer_t *misdnbuf;


struct isdn_msg {
	unsigned long misdn_msg;
  
	enum layer_e layer;
	enum event_e event;
  
	void (*msg_parser)(struct isdn_msg *msgs, msg_t *msg, struct misdn_bchannel *bc, int nt);
	msg_t *(*msg_builder)(struct isdn_msg *msgs, struct misdn_bchannel *bc, int nt);
	char *info;
  
} ; 

/* for isdn_msg_parser.c */
msg_t *create_l3msg(int prim, int mt, int dinfo , int size, int nt);



struct misdn_stack {
	/** is first element because &nst equals &mISDNlist **/
	net_stack_t nst;
	manager_t mgr;
  
	int d_stid;
  
	int b_num;
  
	int b_stids[MAX_BCHANS + 1];
  
	int ptp;

	int l2upcnt;

	int l2_id;
	int lower_id;
	int upper_id;
  

  	int blocked;

	int l2link;
  
	time_t l2establish;
  
	int l1link;
	int midev;
  
	int nt;
	
	int pri;
  

	int procids[0x100+1];

	msg_queue_t downqueue;
	msg_queue_t upqueue;
	int busy;
  
	int port;
	struct misdn_bchannel bc[MAX_BCHANS + 1];
  
	struct misdn_bchannel* bc_list; 
  
	int channels[MAX_BCHANS + 1];

  
	struct misdn_bchannel *holding; /* Queue which holds holded channels :) */
  
	struct misdn_stack *next;
}; 


struct misdn_stack* get_stack_by_bc(struct misdn_bchannel *bc);



#endif
