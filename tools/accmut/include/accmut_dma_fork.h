#ifndef ACCMUT_DMA_FORK_H
#define ACCMUT_DMA_FORK_H

#include<stdlib.h>
#include<string.h>
#include<stdio.h>
#include<unistd.h>
#include<sys/wait.h>

#define MAXMUTNUM 10000

#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>


typedef enum MTYPE{
	AOR,
	LOR,
	COR,
	ROR,
	SOR,
	STD,
	LVR
}MType;

typedef struct Mutation{
	MType type;
	int op;
	//for AOR, LOR
	int t_op;
	//for ROR
	int s_pre;
	int t_pre;
	//for STD
	int f_tp;
	//for LVR
	int op_index;
	long s_con;		// TODO: 'long' is enough ?
	long t_con;
}Mutation;

Mutation* ALLMUTS[MAXMUTNUM + 1];


/** Added By Shiyqw **/

int forked_active_set[21]; 
int forked_active_num;
int default_active_set[MAXMUTNUM + 1];
int recent_set[21];
int recent_num;
long temp_result[21];

typedef struct Eqclass {
    long value;
    int num;
    int mut_id[21];
} Eqclass;

Eqclass eqclass[21];
int eq_num;

// Algorithms for Dynamic mutation anaylsis 

void __accmut__filter__variant(int from, int to) {
    recent_num = 0;
    int i;
    if (MUTATION_ID == 0) {
        recent_set[recent_num++] = 0;
        for(i = from; i <= to; ++i) {
            if (default_active_set[i] == 1) {
                recent_set[recent_num++] = i;
            }
        }
    } else {
        for(i = 0; i < forked_active_num; ++i) {
            if (forked_active_set[i] >= from && forked_active_set[i] <= to) {
                recent_set[recent_num++] = forked_active_set[i];
            }
        }
        if(recent_num == 0) {
            recent_set[recent_num++] = 0;
        }
    }

}
//

void __accmut__divide__eqclass() {
    eq_num = 0;
    int i;
    for(i = 0; i < recent_num; ++i) {
        long result = temp_result[i];
        int j;
        int flag = 0;
        //for(j = 0; j < eq_num; ++j) {
            //if(eqclass[j].value == result) {
                //eqclass[j].mut_id[eqclass[j].num++] = recent_set[i];
                //flag = 1;
                //break;
            //}
        //}
        if (flag == 0) {
            eqclass[eq_num].value = result;
            eqclass[eq_num].num = 1;
            eqclass[eq_num].mut_id[0] = recent_set[i];
            ++eq_num;
        }
    }
}

void __accmut__filter__mutants(int from, int to, int classid) {
    /** filter_mutants **/
    int j;
    if(eqclass[classid].mut_id[0] == 0) {
        for(j = from; j <= to; ++j) {
            default_active_set[j] = 0;
        }
        for(j = 0; j < eqclass[classid].num; ++j) {
           default_active_set[eqclass[classid].mut_id[j]] = 1;
        }
    } else {
        forked_active_num = 0;
        for(j = 0; j < eqclass[classid].num; ++j) {
            forked_active_set[forked_active_num++] = eqclass[classid].mut_id[j];
        }
    }
}

long __accmut__fork__eqclass(int from, int to) {

    if(eq_num == 1) {
        return eqclass[0].value;
    }

    int result = eqclass[0].value;
    int id = eqclass[0].mut_id[0];
    int i;
    
    /** fork **/
    for(i = 1; i < eq_num; ++i) {
         int pid = 0;
         fflush(stdout);
         pid = fork();
         if(pid == 0) {
             int r = setitimer(ITIMER_PROF, &tick, NULL); //  发送信号
             __accmut__filter__mutants(from, to, i);
             MUTATION_ID = eqclass[i].mut_id[0];

             //printf("%d\t%d\n", TEST_ID, MUTATION_ID);

             return eqclass[i].value;
         } else {
             fflush(stdout);
             waitpid(pid, NULL, 0);
         }
    }

    __accmut__filter__mutants(from, to, 0);
    return result;
}


/** End Added **/

void __accmut__init(){

    tick.it_value.tv_sec = VALUE_SEC;  // sec
    tick.it_value.tv_usec = VALUE_USEC; // u sec.
    tick.it_interval.tv_sec = INTTERVAL_SEC;
    tick.it_interval.tv_usec =  INTTERVAL_USEC;

	//perror("######### INIT START ####\n");
    
    signal(SIGPROF, __accmut__handler); 
    char path[100];
    strcpy(path, getenv("HOME"));
    strcat(path, "/tmp/accmut/mutations.txt");
	FILE *fp = fopen(path, "r");
	if(fp == NULL){
		perror("FILE ERROR: mutation.txt CAN NOT OPEN !!!\n");
		exit(0);
	}
	int id;	
	char type[4];
	char buff[50];	
	char useless[20];
	char tail[20];
	while(fgets(buff, 50, fp)){
		//fprintf(stderr, "%s", buff);
		sscanf(buff, "%d:%3s:%*[^:]:%*[^:]:%s", &id, type, tail);
		//fprintf(stderr, "%d -- %s --  %s\n", id, type, tail);
		Mutation* m = (Mutation *)malloc(sizeof(Mutation));
		if(!strcmp(type, "AOR")){
			m->type = AOR;
			int s_op, t_op;
			sscanf(tail, "%d:%d", &s_op, &t_op);
			m->op = s_op;
			m->t_op = t_op;

		}else if(!strcmp(type, "LOR")){
			m->type = LOR;
			int s_op, t_op;
			sscanf(tail, "%d:%d", &s_op, &t_op);
			m->op = s_op;
			m->t_op = t_op;
		}else if(!strcmp(type, "ROR")){
			m->type = ROR;
			int op, s_pre, t_pre;
			sscanf(tail, "%d:%d:%d", &op, &s_pre, &t_pre);
			m->op = op;
			m->s_pre = s_pre;
			m->t_pre = t_pre;
		}else if(!strcmp(type, "STD")){
			m->type = STD;
			int op, f_tp;
 			sscanf(tail, "%d:%d", &op, &f_tp);
			m->op = op;
			m->f_tp=f_tp;
		}else if(!strcmp(type, "LVR")){
			m->type = LVR;
			int op, op_i;
			long s_c, t_c;
			sscanf(tail, "%d:%d:%ld:%ld", &op, &op_i, &s_c, &t_c);
			m->op = op;
            m->t_op = op;
			m->op_index = op_i;
			m->s_con = s_c;
			m->t_con = t_c;
		}	
		ALLMUTS[id] = m;
	}

    int i;
    for(i = 0; i <= MAXMUTNUM; ++i) default_active_set[i] = 1;

	//perror("######### INIT END ####\n");
}

//////

int __accmut__process_call_i32(){
    return 0;
}

long __accmut__process_call_i64(){
	return 0;
}

void __accmut__process_call_void(){

}

///////

int __accmut__cal_i32_arith(int op, int a, int b){// TODO:: add float point
	switch(op){
		case 14: return a + b;
		case 16: return a - b;
		case 18: return a * b;
		case 20: return ((unsigned)a) / ((unsigned)b);
		case 21: return a / b;
		case 23: return ((unsigned)a) % ((unsigned)b);
		case 24: return a % b;
		case 26: return a << b;
		case 27: return ((unsigned)a) >> ((unsigned)b);
		case 28: return a >> b;
		case 29: return a & b;
		case 30: return a | b;
		case 31: return a ^ b;
		default:
			perror("ERROR : __accmut__cal_i32_arith !!!\n");
			exit(0);
	}
}

long __accmut__cal_i64_arith(int op, long a, long b){// TODO:: add float point
	switch(op){
		case 14: return a + b;
		case 16: return a - b;
		case 18: return a * b;
		case 20: return ((unsigned long)a) / ((unsigned long)b);
		case 21: return a / b;
		case 23: return ((unsigned long)a) % ((unsigned long)b);
		case 24: return a % b;
		case 26: return a << b;
		case 27: return ((unsigned long)a) >> ((unsigned long)b);
		case 28: return a >> b;
		case 29: return a & b;
		case 30: return a | b;
		case 31: return a ^ b;
		default:
			perror("ERROR : __accmut__cal_i64_arith !!!\n");
			exit(0);
	}
}


int __accmut__process_i32_arith(int from, int to, int left, int right){
	int ori = __accmut__cal_i32_arith(ALLMUTS[to]->op , left, right);
    
    __accmut__filter__variant(from, to);
    // generate recent_set
    int i;
    for(i = 0; i < recent_num; ++i) {
        if(recent_set[i] == 0) {
            temp_result[i] = ori;
        } else if (ALLMUTS[recent_set[i]]->type == LVR) {
            int op = ALLMUTS[recent_set[i]]->op;
            int t_con = ALLMUTS[recent_set[i]]->t_con;
            if(ALLMUTS[recent_set[i]]->op_index == 0) {
                temp_result[i] = __accmut__cal_i32_arith(op, t_con, right);
            } else {
                temp_result[i] = __accmut__cal_i32_arith(op, left, t_con);
            }
        } else {
            temp_result[i] = __accmut__cal_i32_arith(ALLMUTS[recent_set[i]]->t_op, left, right);
        }
    }

    if(recent_num == 1) {
        if(MUTATION_ID < from || MUTATION_ID > to) {
            return ori;
        }
        return temp_result[0];
    }

    /* divide */
    __accmut__divide__eqclass();
    /* fork */
    int result = __accmut__fork__eqclass(from, to);
    return result;

}

long __accmut__process_i64_arith(int from, int to, long left, long right){

	long ori = __accmut__cal_i64_arith(ALLMUTS[to]->op , left, right);

    __accmut__filter__variant(from, to);

    // generate recent_set
    int i;
    for(i = 0; i < recent_num; ++i) {
        if(recent_set[i] == 0) {
            temp_result[i] = ori;
        } else if (ALLMUTS[recent_set[i]]->type == LVR) {
            int op = ALLMUTS[recent_set[i]]->op;
            long t_con = ALLMUTS[recent_set[i]]->t_con;
            if(ALLMUTS[recent_set[i]]->op_index == 0) {
                temp_result[i] = __accmut__cal_i64_arith(op, t_con, right);
            } else {
                temp_result[i] = __accmut__cal_i64_arith(op, left, t_con);
            }
        } else {
            temp_result[i] = __accmut__cal_i64_arith(ALLMUTS[recent_set[i]]->t_op, left, right);
        }
    }

    if(recent_num == 1) {
        if(MUTATION_ID < from || MUTATION_ID > to) {
            return ori;
        }
        return temp_result[0];
    }
    /* divide */
    __accmut__divide__eqclass();
    /* fork */
    long result = __accmut__fork__eqclass(from, to);
    return result;
}


int __accmut__cal_i32_bool(int pre, int a, int b){
	switch(pre){
		case 32: return a == b;
		case 33: return a != b;
		case 34: return ((unsigned) a) > ((unsigned) b);
		case 35: return ((unsigned) a) >= ((unsigned) b);
		case 36: return ((unsigned) a) < ((unsigned) b);
		case 37: return ((unsigned) a) <= ((unsigned) b);
		case 38: return a > b;
		case 39: return a >= b;
   
		case 40: return a < b;
		case 41: return a <= b;
		default:
			fprintf(stderr, "ERROR : __accmut_cal_i32_bool with %d !!!\n", pre);
			exit(0);
	}
}

int __accmut__cal_i64_bool(int pre, long a, long b){
	switch(pre){
		case 32: return a == b;
		case 33: return a != b;
		case 34: return ((unsigned long) a) > ((unsigned long) b);
		case 35: return ((unsigned long) a) >= ((unsigned long) b);
		case 36: return ((unsigned long) a) < ((unsigned long) b);
		case 37: return ((unsigned long) a) <= ((unsigned long) b);
		case 38: return a > b;
		case 39: return a >= b;
		case 40: return a < b;
		case 41: return a <= b;
		default:
			fprintf(stderr, "ERROR : __accmut__cal_i64_bool !!!\n");
			exit(0);
	}
}

int __accmut__process_i32_cmp(int from, int to, int left, int right){
	int ori = __accmut__cal_i32_bool(ALLMUTS[to]->s_pre , left, right);
    
    __accmut__filter__variant(from, to);

    // generate recent_set
    int i;
    for(i = 0; i < recent_num; ++i) {
        if(recent_set[i] == 0) {
            temp_result[i] = ori;
        } else if (ALLMUTS[recent_set[i]]->type == LVR) {
            int pre = ALLMUTS[to]->s_pre;
            int t_con = ALLMUTS[recent_set[i]]->t_con;
            if(ALLMUTS[recent_set[i]]->op_index == 0) {
                temp_result[i] = __accmut__cal_i32_bool(pre, t_con, right);
            } else {
                temp_result[i] = __accmut__cal_i32_bool(pre, left, t_con);
            }
        } else {
            temp_result[i] = __accmut__cal_i32_bool(ALLMUTS[recent_set[i]]->t_pre, left, right);
        }
    }
    if(recent_num == 1) {
        if(MUTATION_ID < from || MUTATION_ID > to) {
            return ori;
        }
        return temp_result[0];
    }
    /* divide */
    __accmut__divide__eqclass();
    /* fork */
    //int result = ori;
    int result = __accmut__fork__eqclass(from, to);
    return result;
}

int __accmut__process_i64_cmp(int from, int to, long left, long right){
	long ori = __accmut__cal_i64_bool(ALLMUTS[to]->s_pre , left, right);

    __accmut__filter__variant(from, to);

    // generate recent_set
    int i;
    for(i = 0; i < recent_num; ++i) {
        if(recent_set[i] == 0) {
            temp_result[i] = ori;
        } else if (ALLMUTS[recent_set[i]]->type == LVR) {
            int pre = ALLMUTS[to]->s_pre;
            long t_con = ALLMUTS[recent_set[i]]->t_con;
            if(ALLMUTS[recent_set[i]]->op_index == 0) {
                temp_result[i] = __accmut__cal_i64_bool(pre, t_con, right);
            } else {
                temp_result[i] = __accmut__cal_i64_bool(pre, left, t_con);
            }
        } else {
            temp_result[i] = __accmut__cal_i64_bool(ALLMUTS[recent_set[i]]->t_pre, left, right);
        }
    }

    if(recent_num == 1) {
        if(MUTATION_ID < from || MUTATION_ID > to) {
            return ori;
        }
        return temp_result[0];
    }
    /* divide */
    __accmut__divide__eqclass();
    /* fork */
    long result = __accmut__fork__eqclass(from, to);
    return result;
}

void __accmut__process_st_i32(int from, int to, int *addr){
	int ori = ALLMUTS[to]->s_con;

    __accmut__filter__variant(from, to);

    // generate recent_set
    int i;
    for(i = 0; i < recent_num; ++i) {
        if(recent_set[i] == 0) {
            temp_result[i] = ori;
        } else {
            int t_con = ALLMUTS[recent_set[i]]->t_con;
            temp_result[i] = t_con;
        }
    }

    if(recent_num == 1) {
        if(MUTATION_ID < from || MUTATION_ID > to) {
            *addr = ori;
            return;
        }
        *addr = temp_result[0];
        return;
    }
    /* divide */
    __accmut__divide__eqclass();
    /* fork */
    int result = __accmut__fork__eqclass(from, to);
	//orig	
	*addr = result;
}

void __accmut__process_st_i64(int from, int to, long *addr){
	long ori = ALLMUTS[to]->s_con;

    __accmut__filter__variant(from, to);

    if(recent_num == 1) {
        return;
    }
    // generate recent_set
    int i;
    for(i = 0; i < recent_num; ++i) {
        if(recent_set[i] == 0) {
            temp_result[i] = ori;
        } else {
            long t_con = ALLMUTS[recent_set[i]]->t_con;
            temp_result[i] = t_con;
        }
    }

    /* divide */
    __accmut__divide__eqclass();
    /* fork */
    long result = __accmut__fork__eqclass(from, to);
	//orig	
	*addr = result;
}

#endif