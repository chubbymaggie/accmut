//===----------------------------------------------------------------------===//
//
// This file decribes the sattic mutation analysis IR instrumenter pass
// 
// Add by Wang Bo. DEC 23, 2015
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/raw_ostream.h"

#include "llvm/Transforms/AccMut/SMAInstrumenter.h"
#include "llvm/Transforms/AccMut/MutUtil.h"


#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"


#include<fstream>
#include<sstream>
#include<string>
#include<cstdlib>

#include "clang/AST/Stmt.h"
#include "llvm/IR/InstIterator.h"

using namespace llvm;
using namespace std;

#if 0
using clang::Stmt;

extern std::map<Value*, Stmt*> toStmtMap;

map<Stmt*, bool> mutatedStmt;	 // TODO:: change to std::set
map<Instruction*, int> instToExprID;
vector<Instruction *> implementedList;

ofstream SMAInstrumenter::expr_os;
ofstream SMAInstrumenter::mut_os;
#endif

#if 0
SMAInstrumenter::SMAInstrumenter(Module *M) : FunctionPass(ID) {
	this->TheModule = M;
	MutUtil::getAllMutations();
	#if 0
	string home = getenv("HOME");
	stringstream ss;
	ss<<home<<"/tmp/accmut/expr.txt"; 
	expr_os.open(ss.str(), ios::trunc); 
	stringstream ss2;
	ss2<<home<<"/tmp/accmut/mut.txt"; 
	mut_os.open(ss2.str(), ios::trunc); 
	#endif
}


bool SMAInstrumenter::runOnFunction(Function & F){
        if(F.getName().startswith("__accmut__")){
                return false;
        }
        if(F.getName().equals("main")){
                return true;
        }
        vector<Mutation*>* v= MutUtil::AllMutsMap[F.getName()];
        
        if(v == NULL || v->size() == 0){
                return false;
        }

        llvm::errs()<<"\n######## SMA INSTRUMTNTING MUT  @"<<TheModule->getName()<<"->"<<F.getName()<<"()  ########\n\n";     

        instrument(F, v);

        return true;
}


void SMAInstrumenter::instrument(Function &F, vector<Mutation*> * v){

        int instrumented_insts = 0;
        
        Function::iterator cur_bb;
        BasicBlock::iterator cur_it;

        for(unsigned i = 0; i < v->size(); i++){
                vector<Mutation*> tmp;
                Mutation* m = (*v)[i];
                tmp.push_back(m);
                //collect all mutations of one instruction
                for(unsigned j = i + 1; j < v->size(); j++){
                        Mutation* m1 = (*v)[j];
                        if(m1->index == m->index){
                                tmp.push_back(m1);
                                i = j;          // TODO: check
                        }else{
                                break;
                        }
                }
                
                cur_it =  getLocation(F, instrumented_insts, tmp[0]->index);

                cur_bb = cur_it->getParent();

                int mut_from, mut_to;
                mut_from = tmp.front()->id;
                mut_to = tmp.back()->id;

                if(tmp.size() >= MAX_MUT_NUM_PER_LOCATION){
                        llvm::errs()<<"TOO MANY MUTS@ "<<__FUNCTION__<<"() : "<<__LINE__<<"\n";
                        llvm::errs()<<"CUR_INST: "<<tmp.front()->index<<"\t(FROM: "
                                <<mut_from<<"\tTO: "<<mut_to<<")\t"<<*cur_it<<"\n";
                        exit(0);
                }
                
                llvm::errs()<<"CUR_INST: "<<tmp.front()->index<<"\t(FROM: "
                        <<mut_from<<"\tTO: "<<mut_to<<")\t"<<*cur_it<<"\n";

#if (!ACCMUT_STATIC_ANALYSIS_INSTRUMENT_EVAL)

                if(dyn_cast<CallInst>(&*cur_it)){
                        //move all constant literal int to repalce to alloca
                        for (auto OI = cur_it->op_begin(), OE = cur_it->op_end(); OI != OE; ++OI){
                                if(ConstantInt* cons = dyn_cast<ConstantInt>(&*OI)){
                                        AllocaInst *alloca = new AllocaInst(cons->getType(), "cons_alias", cur_it);
                                        StoreInst *str = new StoreInst(cons, alloca, cur_it);
                                        LoadInst *ld = new LoadInst(alloca, "const_load", cur_it);
                                        *OI = (Value*) ld;
                                        instrumented_insts += 3;//add 'alloca', 'store' and 'load'
                                }
                        }

                        
                        Function* precallfunc = TheModule->getFunction("__accmut__prepare_call");
                        std::vector<Value*> params;
                        std::stringstream ss;
                        ss<<mut_from;
                        ConstantInt* from_i32= ConstantInt::get(TheModule->getContext(), 
                                        APInt(32, StringRef(ss.str()), 10)); 
                        params.push_back(from_i32);
                        ss.str("");
                        ss<<mut_to;
                        ConstantInt* to_i32= ConstantInt::get(TheModule->getContext(),
                                APInt(32, StringRef(ss.str()), 10));
                        params.push_back(to_i32);

                        int index = 0;
                        int record_num = 0;
                        //get signature info
                        for (auto OI = cur_it->op_begin(), OE = cur_it->op_end(); OI != OE; ++OI, ++index){
                                Type* OIType = (dyn_cast<Value>(&*OI))->getType();
                                int tp = getTypeMacro(OIType);
                                if(tp < 0){
                                        continue;
                                }
                                //push type
                                ss.str("");
                                short tp_and_idx = ((unsigned char)tp)<<8;
                                tp_and_idx = tp_and_idx | index;
                                ss<<tp_and_idx;
                                ConstantInt* ctai = ConstantInt::get(TheModule->getContext(), 
                                        APInt(16, StringRef(ss.str()), 10)); 
                                params.push_back(ctai);
                                //now push the pointer of idx'th param

                                // TODO:: for array ! 
                                if(LoadInst *ld = dyn_cast<LoadInst>(&*OI)){//is a local var
                                        params.push_back(ld->getPointerOperand());
                                }else if(AllocaInst *alloca = dyn_cast<AllocaInst>(&*OI)){// a param of the F, fetch it by alloca
                                        params.push_back(alloca);
                                }else{
                                        llvm::errs()<<"NOT A POINTER @ "<<__FUNCTION__<<"() : "<<__LINE__<<"\n";
                                        exit(0);
                                }
                                record_num++;
                        }
                        //insert num of param-records
                        ss.str("");
                        ss<<record_num;
                        ConstantInt* rcd = ConstantInt::get(TheModule->getContext(), 
                                        APInt(32, StringRef(ss.str()), 10)); 
                        params.insert( params.begin()+2 , rcd);

                        CallInst *pre = CallInst::Create(precallfunc, params, "", cur_it);
                        pre->setCallingConv(CallingConv::C);
                        pre->setTailCall(false);
                        AttributeSet preattrset;
                        pre->setAttributes(preattrset);
                        
                        ConstantInt* zero = ConstantInt::get(Type::getInt32Ty(TheModule->getContext()), 0);
                        
                        ICmpInst *hasstd = new ICmpInst(cur_it, ICmpInst::ICMP_EQ, pre, zero, "hasstd");
                        
                        BasicBlock *cur_bb = cur_it->getParent();
                        
                        Instruction* oricall = cur_it->clone();
                        
                        BasicBlock* label_if_end = cur_bb->splitBasicBlock(cur_it, "if.end");
                        
                        BasicBlock* label_if_then = BasicBlock::Create(TheModule->getContext(), "if.then",cur_bb->getParent(), label_if_end);
                        BasicBlock* label_if_else = BasicBlock::Create(TheModule->getContext(), "if.else",cur_bb->getParent(), label_if_end);
                        
                        cur_bb->back().eraseFromParent();
                        
                        BranchInst::Create(label_if_then, label_if_else, hasstd, cur_bb);
                                
                        //label_if_then
                        //move the loadinsts of params into if_then_block
                        for (auto OI = oricall->op_begin(), OE = oricall->op_end() - 1; OI != OE; ++OI){
                                if(LoadInst *ld = dyn_cast<LoadInst>(&*OI)){
                                        ld->removeFromParent();
                                        label_if_then->getInstList().push_back(ld);
                                }else if(Constant *con = dyn_cast<Constant>(&*OI)){// TODO:: test
                                        continue;
                                }else{
                                        llvm::errs()<<"NOT A LoadInst @ "<<__FUNCTION__<<"() : "<<__LINE__<<"\n";
                                        cur_it->dump();
                                        Value *v = dyn_cast<Value>(&*OI);
                                        v->dump();
                                        exit(0);
                                }
                        }
                        label_if_then->getInstList().push_back(oricall);
                        BranchInst::Create(label_if_end, label_if_then);        
                        
                        //label_if_else
                        Function *std_handle;
                        if(oricall->getType()->isIntegerTy(32)){
                                std_handle = TheModule->getFunction("__accmut__stdcall_i32");
                        }else if(oricall->getType()->isIntegerTy(64)){
                                std_handle = TheModule->getFunction("__accmut__stdcall_i64");
                        }else if(oricall->getType()->isVoidTy()){
                                std_handle = TheModule->getFunction("__accmut__stdcall_void");
                        }else{
                                llvm::errs()<<"ERR CALL TYPE @ "<<__FUNCTION__<<"() : "<<__LINE__<<"\n";
                                exit(0);
                        }
                        
                        CallInst*  stdcall = CallInst::Create(std_handle, "", label_if_else);
                        stdcall->setCallingConv(CallingConv::C);
                        stdcall->setTailCall(false);
                        AttributeSet stdcallPAL;
                        stdcall->setAttributes(stdcallPAL);
                        BranchInst::Create(label_if_end, label_if_else);
                        
                        //label_if_end
                        if(oricall->getType()->isVoidTy()){
                                cur_it->eraseFromParent();
                                instrumented_insts += 6;
                        }
                        else{
                                PHINode* call_res = PHINode::Create(IntegerType::get(TheModule->getContext(), 32), 2, "call.phi");
                                call_res->addIncoming(oricall, label_if_then);
                                call_res->addIncoming(stdcall, label_if_else);
                                ReplaceInstWithInst(cur_it, call_res);
                                instrumented_insts += 7;
                        }

                }
                
                else if(StoreInst* st = dyn_cast<StoreInst>(&*cur_it)){
                        if(ConstantInt* cons = dyn_cast<ConstantInt>(st->getValueOperand())){
                                        AllocaInst *alloca = new AllocaInst(cons->getType(), "cons_alias", st);
                                        StoreInst *str = new StoreInst(cons, alloca, st);
                                        LoadInst *ld = new LoadInst(alloca, "const_load", st);
                                        User::op_iterator OI = st->op_begin();
                                        *OI = (Value*) ld;
                                        instrumented_insts += 3;
                        }

                        Function* prestfunc;
                        if(st->getValueOperand()->getType()->isIntegerTy(32)){
                                prestfunc = TheModule->getFunction("__accmut__prepare_st_i32");
                        }
                        else if(st->getValueOperand()->getType()->isIntegerTy(64)){
                                prestfunc = TheModule->getFunction("__accmut__prepare_st_i64");
                        }else{
                                llvm::errs()<<"ERR STORE TYPE @ "<<__FUNCTION__<<"() : "<<__LINE__<<"\n";
                                exit(0);
                        }
                        
                        std::vector<Value*> params;
                        std::stringstream ss;
                        ss<<mut_from;
                        ConstantInt* from_i32= ConstantInt::get(TheModule->getContext(), 
                                        APInt(32, StringRef(ss.str()), 10)); 
                        params.push_back(from_i32);
                        ss.str("");
                        ss<<mut_to;
                        ConstantInt* to_i32= ConstantInt::get(TheModule->getContext(),
                                APInt(32, StringRef(ss.str()), 10));
                        params.push_back(to_i32);

                        Value* tobestored = dyn_cast<Value>(st->op_begin());
                        params.push_back(tobestored);
                        
                        auto addr = st->op_begin() + 1;// the pointer of the storeinst
                        if(LoadInst *ld = dyn_cast<LoadInst>(&*addr)){//is a local var
                                params.push_back(ld->getPointerOperand());
                        }else if(AllocaInst *alloca = dyn_cast<AllocaInst>(&*addr)){
                                params.push_back(alloca);
                        }else if(Constant *con = dyn_cast<Constant>(&*addr)){
                                params.push_back(con);
                        }else{
                                llvm::errs()<<"NOT A POINTER @ "<<__FUNCTION__<<"() : "<<__LINE__<<"\n";
                                cur_it->dump();
                                Value *v = dyn_cast<Value>(&*addr);
                                v->dump();
                                exit(0);
                        }
                        CallInst *pre = CallInst::Create(prestfunc, params, "", cur_it);
                        pre->setCallingConv(CallingConv::C);
                        pre->setTailCall(false);
                        AttributeSet attrset;
                        pre->setAttributes(attrset);

                        ConstantInt* zero = ConstantInt::get(Type::getInt32Ty(TheModule->getContext()), 0);
                
                        ICmpInst *hasstd = new ICmpInst(cur_it, ICmpInst::ICMP_EQ, pre, zero, "hasstd");
                
                        BasicBlock *cur_bb = cur_it->getParent();
                                                
                        BasicBlock* label_if_end = cur_bb->splitBasicBlock(cur_it, "if.end");
                        
                        BasicBlock* label_if_else = BasicBlock::Create(TheModule->getContext(), "std.st",cur_bb->getParent(), label_if_end);
                        
                        cur_bb->back().eraseFromParent();
                        
                        BranchInst::Create(label_if_end, label_if_else, hasstd, cur_bb);


                        //label_if_else
                        Function *std_handle = TheModule->getFunction("__accmut__std_store");
                        CallInst*  stdcall = CallInst::Create(std_handle, "", label_if_else);
                        stdcall->setCallingConv(CallingConv::C);
                        stdcall->setTailCall(false);
                        AttributeSet stdcallPAL;
                        stdcall->setAttributes(stdcallPAL);
                        BranchInst::Create(label_if_end, label_if_else);

                        //label_if_end
                        cur_it->eraseFromParent();      
                        instrumented_insts += 4;
                }
                else{
#endif
					
                    // FOR ARITH INST
                    if(cur_it->getOpcode() >= Instruction::Add && 
                            cur_it->getOpcode() <= Instruction::Xor){ 
                            Type* ori_ty = cur_it->getType();
                            Function* f_process;
                            if(ori_ty->isIntegerTy(32)){
                                    f_process = TheModule->getFunction("__accmut__process_i32_arith");
                            }else if(ori_ty->isIntegerTy(64)){
                                    f_process = TheModule->getFunction("__accmut__process_i64_arith");
                            }else{
                                    llvm::errs()<<"ArithInst TYPE ERROR @ "<<__FUNCTION__<<"() : "<<__LINE__<<"\n";
                                    cur_it->dump();
                                    llvm::errs()<<*ori_ty<<"\n";
                                    // TODO:: handle i1, i8, i64 ... type
                                    exit(0);
                            }

                            std::vector<Value*> int_call_params;
                            std::stringstream ss;
                            ss<<mut_from;
                            ConstantInt* from_i32= ConstantInt::get(TheModule->getContext(), APInt(32, StringRef(ss.str()), 10)); 
                            int_call_params.push_back(from_i32);
                            ss.str("");
                            ss<<mut_to;
                            ConstantInt* to_i32= ConstantInt::get(TheModule->getContext(), APInt(32, StringRef(ss.str()), 10));
                            int_call_params.push_back(to_i32);
                            int_call_params.push_back(cur_it->getOperand(0));
                            int_call_params.push_back(cur_it->getOperand(1));
                            CallInst *call = CallInst::Create(f_process, int_call_params);
                            ReplaceInstWithInst(cur_it, call);

                            
                    }
                    // FOR ICMP INST
                    else if(cur_it->getOpcode() == Instruction::ICmp){
                            Function* f_process;
                            
                            if(cur_it->getOperand(0)->getType()->isIntegerTy(32)){
                                    f_process = TheModule->getFunction("__accmut__process_i32_cmp");
                            }else if(cur_it->getOperand(0)->getType()->isIntegerTy(64)){
                                    f_process = TheModule->getFunction("__accmut__process_i64_cmp");
                            }else{
                                    llvm::errs()<<"TYPE ERROR @ instrument() ICmpInst\n";                   
                                    exit(0);
                            }
                            
                            std::vector<Value*> int_call_params;
                            std::stringstream ss;
                            ss<<mut_from;
                            ConstantInt* from_i32= ConstantInt::get(TheModule->getContext(), 
                                            APInt(32, StringRef(ss.str()), 10)); 
                            int_call_params.push_back(from_i32);
                            ss.str("");
                            ss<<mut_to;
                            ConstantInt* to_i32= ConstantInt::get(TheModule->getContext(),
                                    APInt(32, StringRef(ss.str()), 10));
                            int_call_params.push_back(to_i32);
                            int_call_params.push_back(cur_it->getOperand(0));
                            int_call_params.push_back(cur_it->getOperand(1));
                            CallInst *call = CallInst::Create(f_process, int_call_params, "", cur_it);
                            CastInst* i32_conv = new TruncInst(call, IntegerType::get(TheModule->getContext(), 1), "");

                            instrumented_insts++;
                            
                            ReplaceInstWithInst(cur_it, i32_conv);
                        }

#if (!ACCMUT_STATIC_ANALYSIS_INSTRUMENT_EVAL)
                }
#endif
				
                
        }
}


BasicBlock::iterator SMAInstrumenter::getLocation(Function &F, int instrumented_insts, int index){
	int cur = 0;
	for(Function::iterator FI = F.begin(); FI != F.end(); ++FI){
		BasicBlock *BB = FI;
		for(BasicBlock::iterator BI = BB->begin(); BI != BB->end(); ++BI, cur++){
			if(index + instrumented_insts == cur ){
				return BI;
			}
		}
	}
	return F.back().end();
}


/*------------------reserved begin-------------------*/
void SMAInstrumenter::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

char SMAInstrumenter::ID = 0;
/*-----------------reserved end --------------------*/

#endif
