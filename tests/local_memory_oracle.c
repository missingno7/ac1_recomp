#include "cpu.h"
#include <stdio.h>
#include <assert.h>
#include <signal.h>
uint8_t *ac1_direct_arena;
void run_legacy(CPU *,uint32_t);
void run_local(CPU *,uint32_t);
void fail_legacy(CPU *,uint32_t,unsigned);
void fail_local(CPU *,uint32_t,unsigned);
void ac1_unsupported_instruction(uint32_t a,const char *reason){fprintf(stderr,"unexpected unsupported %x %s\n",a,reason);abort();}
/* Expected failure probes must not launch an OS crash-reporting dialog. */
static void expected_abort(int signal_number){(void)signal_number;_Exit(3);}

int main(int argc, char **argv)
{
    signal(SIGABRT,expected_abort);
    guest_address_space *spaces[2]={guest_address_space_create_direct(),guest_address_space_create_direct()};
    assert(spaces[0]&&spaces[1]);
    uint8_t *mapped[2];
    for(unsigned j=0;j<2;++j){
        guest_address_space_select(spaces[j]);
        mapped[j]=guest_memory_allocate(0x8000,0x1000);
        assert(mapped[j]&&guest_memory_allocate(0x9000,0x1000));
    }
    if(argc==4){
        unsigned local=(unsigned)strtoul(argv[1],NULL,0);
        guest_address_space_select(spaces[local]);ac1_direct_arena=guest_execution_base;
        CPU cpu={0};uint32_t address=(uint32_t)strtoul(argv[3],NULL,0);
        unsigned operation=(unsigned)strtoul(argv[2],NULL,0);
        if(local)fail_local(&cpu,address,operation);else fail_legacy(&cpu,address,operation);
        fputs("unexpected atomic success\n",stderr);return 4;
    }
    uint32_t random=0xcafe23;
    for(unsigned i=0;i<4096;++i){
        CPU cpu[2];memset(cpu,0,sizeof(cpu));
        random=random*1664525u+1013904223u;
        for(unsigned j=0;j<2;++j){
            guest_address_space_select(spaces[j]);ac1_direct_arena=guest_execution_base;
            for(unsigned k=0;k<0x2000;++k)mapped[j][k]=(uint8_t)(random+k*13u);
            if(j)run_local(&cpu[j],random);else run_legacy(&cpu[j],random);
        }
        if(memcmp(cpu,cpu+1,sizeof(CPU))||memcmp(mapped[0],mapped[1],0x2000)){
            fprintf(stderr,"captured-base mismatch: iteration=%u seed=%08x\n",i,random);return 1;
        }
    }
    ac1_direct_arena=NULL;
    for(unsigned j=0;j<2;++j)guest_address_space_destroy(spaces[j]);
    puts("4096 full CPU/memory cases passed: scalar, SSE/MMX bits, ext80, x87 env/save/restore, stack, aligned atomics, adjacent mappings and changed arena between invocations");
    return 0;
}
