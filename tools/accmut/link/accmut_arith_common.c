
#include <limits.h>

#include "accmut_arith_common.h"
#include "accmut_config.h"
#include "accmut_exitcode.h"

#define __real_fprintf fprintf

int __accmut__cal_i32_arith(int op, int a, int b){// TODO:: add float point
	switch(op){
		case 14: return a + b;
		case 16: return a - b;
		case 18: return a * b;
		case 20: {
			if(b == 0) 
				return INT_MAX;
			return ((unsigned)a) / ((unsigned)b);
		}
		case 21:{
			if(b == 0) 
				return INT_MAX;
			return a / b;
		}
		case 23:{
			if(b == 0) 
				return INT_MAX;
			return ((unsigned)a) % ((unsigned)b);
		} 
		case 24: {
			if(b == 0) 
				return INT_MAX;
			return a % b;
		}
		case 26: return a << b;
		case 27: return ((unsigned)a) >> ((unsigned)b);
		case 28: return a >> b;
		case 29: return a & b;
		case 30: return a | b;
		case 31: return a ^ b;
		default:
			ERRMSG("opcode ERR ");
			exit(OPCD_ERR);
	}
}

long __accmut__cal_i64_arith(int op, long a, long b){// TODO:: add float point
	switch(op){
		case 14: return a + b;
		case 16: return a - b;
		case 18: return a * b;
		case 20: {
			if(b == 0) 
				return LONG_MAX;
			return ((unsigned long)a) / ((unsigned long)b);
		}
		case 21: {
			if(b == 0) 
				return LONG_MAX;
			return a / b;
		}
		case 23: {
			if(b == 0) 
				return LONG_MAX;
			return ((unsigned long)a) % ((unsigned long)b);
		}
		case 24: {
			if(b == 0) 
				return LONG_MAX;
			return a % b;
		}
		case 26: return a << b;
		case 27: return ((unsigned long)a) >> ((unsigned long)b);
		case 28: return a >> b;
		case 29: return a & b;
		case 30: return a | b;
		case 31: return a ^ b;
		default:
			ERRMSG("opcode ERR ");
			exit(OPCD_ERR);
	}
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
			ERRMSG("pre ERR ");
			exit(OPCD_ERR);
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
			ERRMSG("pre ERR ");
			exit(OPCD_ERR);
	}
}

