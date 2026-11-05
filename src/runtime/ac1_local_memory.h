#pragma once
#if defined(AC1_DIRECT_MEMORY)
#define AC1_DECLARE_LOCAL_ARENA uint8_t * const _ac1_local_arena = ac1_direct_arena; (void)_ac1_local_arena
#undef ac1_direct_atomic_cell
#define ac1_direct_atomic_cell(...) ac1_base_ac1_direct_atomic_cell(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_direct_cmpxchg32
#define ac1_direct_cmpxchg32(...) ac1_base_ac1_direct_cmpxchg32(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_direct_read
#define ac1_direct_read(...) ac1_base_ac1_direct_read(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_direct_write
#define ac1_direct_write(...) ac1_base_ac1_direct_write(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_direct_xadd32
#define ac1_direct_xadd32(...) ac1_base_ac1_direct_xadd32(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_direct_xchg32
#define ac1_direct_xchg32(...) ac1_base_ac1_direct_xchg32(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_x87_load_env
#define ac1_x87_load_env(...) ac1_base_ac1_x87_load_env(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_x87_restore
#define ac1_x87_restore(...) ac1_base_ac1_x87_restore(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_x87_save
#define ac1_x87_save(...) ac1_base_ac1_x87_save(_ac1_local_arena, c, __VA_ARGS__)
#undef ac1_x87_store_env
#define ac1_x87_store_env(...) ac1_base_ac1_x87_store_env(_ac1_local_arena, c, __VA_ARGS__)
#undef atomic_cmpxchg32
#define atomic_cmpxchg32(...) ac1_base_atomic_cmpxchg32(_ac1_local_arena, c, __VA_ARGS__)
#undef atomic_xadd32
#define atomic_xadd32(...) ac1_base_atomic_xadd32(_ac1_local_arena, c, __VA_ARGS__)
#undef atomic_xchg32
#define atomic_xchg32(...) ac1_base_atomic_xchg32(_ac1_local_arena, c, __VA_ARGS__)
#undef bit_string_op
#define bit_string_op(...) ac1_base_bit_string_op(_ac1_local_arena, c, __VA_ARGS__)
#undef guest_atomic_cmpxchg32
#define guest_atomic_cmpxchg32(...) ac1_base_ac1_direct_cmpxchg32(_ac1_local_arena, c, __VA_ARGS__)
#undef guest_atomic_xadd32
#define guest_atomic_xadd32(...) ac1_base_ac1_direct_xadd32(_ac1_local_arena, c, __VA_ARGS__)
#undef guest_atomic_xchg32
#define guest_atomic_xchg32(...) ac1_base_ac1_direct_xchg32(_ac1_local_arena, c, __VA_ARGS__)
#undef guest_execution_read
#define guest_execution_read(...) ac1_base_ac1_direct_read(_ac1_local_arena, c, __VA_ARGS__)
#undef guest_execution_write
#define guest_execution_write(...) ac1_base_ac1_direct_write(_ac1_local_arena, c, __VA_ARGS__)
#undef pop32
#define pop32(...) ac1_base_pop32(_ac1_local_arena, c, __VA_ARGS__)
#undef push32
#define push32(...) ac1_base_push32(_ac1_local_arena, c, __VA_ARGS__)
#undef rd16
#define rd16(...) ac1_base_rd16(_ac1_local_arena, c, __VA_ARGS__)
#undef rd32
#define rd32(...) ac1_base_rd32(_ac1_local_arena, c, __VA_ARGS__)
#undef rd64
#define rd64(...) ac1_base_rd64(_ac1_local_arena, c, __VA_ARGS__)
#undef rd8
#define rd8(...) ac1_base_rd8(_ac1_local_arena, c, __VA_ARGS__)
#undef rdf32
#define rdf32(...) ac1_base_rdf32(_ac1_local_arena, c, __VA_ARGS__)
#undef rdf64
#define rdf64(...) ac1_base_rdf64(_ac1_local_arena, c, __VA_ARGS__)
#undef rdf80
#define rdf80(...) ac1_base_rdf80(_ac1_local_arena, c, __VA_ARGS__)
#undef rdi16
#define rdi16(...) ac1_base_rdi16(_ac1_local_arena, c, __VA_ARGS__)
#undef rdi32
#define rdi32(...) ac1_base_rdi32(_ac1_local_arena, c, __VA_ARGS__)
#undef rdi64
#define rdi64(...) ac1_base_rdi64(_ac1_local_arena, c, __VA_ARGS__)
#undef rdsd
#define rdsd(...) ac1_base_rdsd(_ac1_local_arena, c, __VA_ARGS__)
#undef rdss
#define rdss(...) ac1_base_rdss(_ac1_local_arena, c, __VA_ARGS__)
#undef rdxm
#define rdxm(...) ac1_base_rdxm(_ac1_local_arena, c, __VA_ARGS__)
#undef wr16
#define wr16(...) ac1_base_wr16(_ac1_local_arena, c, __VA_ARGS__)
#undef wr32
#define wr32(...) ac1_base_wr32(_ac1_local_arena, c, __VA_ARGS__)
#undef wr64
#define wr64(...) ac1_base_wr64(_ac1_local_arena, c, __VA_ARGS__)
#undef wr8
#define wr8(...) ac1_base_wr8(_ac1_local_arena, c, __VA_ARGS__)
#undef wrf32
#define wrf32(...) ac1_base_wrf32(_ac1_local_arena, c, __VA_ARGS__)
#undef wrf64
#define wrf64(...) ac1_base_wrf64(_ac1_local_arena, c, __VA_ARGS__)
#undef wrf80
#define wrf80(...) ac1_base_wrf80(_ac1_local_arena, c, __VA_ARGS__)
#undef wri16
#define wri16(...) ac1_base_wri16(_ac1_local_arena, c, __VA_ARGS__)
#undef wri32
#define wri32(...) ac1_base_wri32(_ac1_local_arena, c, __VA_ARGS__)
#undef wri64
#define wri64(...) ac1_base_wri64(_ac1_local_arena, c, __VA_ARGS__)
#undef wrsd
#define wrsd(...) ac1_base_wrsd(_ac1_local_arena, c, __VA_ARGS__)
#undef wrss
#define wrss(...) ac1_base_wrss(_ac1_local_arena, c, __VA_ARGS__)
#undef wrxm
#define wrxm(...) ac1_base_wrxm(_ac1_local_arena, c, __VA_ARGS__)
#else
#define AC1_DECLARE_LOCAL_ARENA ((void)0)
#endif
