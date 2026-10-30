// Mach subset on Linux (README.md): types, time, task / thread queries and the VM calls the host makes
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef int kern_return_t;
typedef unsigned mach_port_t, thread_act_t, task_t, mach_msg_type_number_t, natural_t;
typedef thread_act_t *thread_act_array_t;
typedef uintptr_t vm_address_t, vm_offset_t, vm_size_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef int vm_prot_t, boolean_t, thread_flavor_t, task_flavor_t, thread_policy_flavor_t;
typedef int *thread_info_t, *task_info_t, *thread_state_t, *vm_region_info_t, *thread_policy_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define TRUE 1
#define FALSE 0
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
extern vm_size_t vm_page_size;
mach_port_t mach_task_self(void);
mach_port_t mach_thread_self(void);
mach_port_t pthread_mach_thread_np(pthread_t t);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
// time: mach_absolute_time counts nanoseconds (timebase 1/1)
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t, *mach_timebase_info_t;
uint64_t mach_absolute_time(void);
kern_return_t mach_timebase_info(mach_timebase_info_t info);
kern_return_t mach_wait_until(uint64_t deadline);
// memory: reads and writes that fail on unmapped memory instead of faulting (process_vm_readv / writev), protection changes, the rest unavailable
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size, vm_address_t data, vm_size_t *out);
kern_return_t vm_write(mach_port_t task, vm_address_t addr, vm_offset_t data, mach_msg_type_number_t cnt);
kern_return_t vm_protect(mach_port_t task, vm_address_t addr, vm_size_t size, boolean_t set_max, vm_prot_t prot);
kern_return_t vm_allocate(mach_port_t task, vm_address_t *addr, vm_size_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size);
kern_return_t vm_remap(mach_port_t task, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t src_task, vm_address_t from, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, int inherit);
// thread / task introspection (diagnostics): unavailable
typedef struct { struct { int seconds, microseconds; } user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / sizeof(int)))
#define TH_STATE_RUNNING 1
#define TH_STATE_WAITING 3
kern_return_t thread_info(mach_port_t th, thread_flavor_t flavor, thread_info_t out, mach_msg_type_number_t *cnt);
kern_return_t thread_suspend(mach_port_t th);
kern_return_t thread_resume(mach_port_t th);
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(int)))
kern_return_t thread_get_state(mach_port_t th, int flavor, thread_state_t st, mach_msg_type_number_t *cnt);
kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *n);
typedef struct { uint64_t virtual_size, region_count, page_size, resident_size, resident_size_peak, device, device_peak, internal, internal_peak, external, external_peak, reusable, reusable_peak,
                 purgeable_volatile_pmap, purgeable_volatile_resident, purgeable_volatile_virtual, compressed, compressed_peak, compressed_lifetime, phys_footprint; } task_vm_info_data_t;
typedef struct { uint64_t total_user, total_system, task_interrupt_wakeups, task_platform_idle_wakeups, task_timer_wakeups_bin_1, task_timer_wakeups_bin_2; } task_power_info_data_t;
typedef struct { uint64_t task_gpu_utilisation, task_gpu_stat_reserved0, task_gpu_stat_reserved1, task_gpu_stat_reserved2; } gpu_energy_data;
typedef struct { task_power_info_data_t cpu_energy; gpu_energy_data gpu_energy; uint64_t task_energy, task_ptime, task_pset_switches; } task_power_info_v2_data_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
#define TASK_POWER_INFO_V2 26
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(task_power_info_v2_data_t) / sizeof(int)))
kern_return_t task_info(mach_port_t task, task_flavor_t flavor, task_info_t out, mach_msg_type_number_t *cnt);
// thread_policy: time-constraint scheduling is not available to apps on Android
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / sizeof(int)))
kern_return_t thread_policy_set(mach_port_t th, thread_policy_flavor_t flavor, thread_policy_t policy, mach_msg_type_number_t cnt);
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
// VM regions (diagnostics): unavailable
#define MACH_PORT_NULL 0u
typedef struct { vm_prot_t protection, max_protection; int inheritance, shared, reserved; uint64_t offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
kern_return_t vm_region_64(mach_port_t task, vm_address_t *addr, vm_size_t *size, int flavor, vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj);
kern_return_t thread_policy_get(mach_port_t th, thread_policy_flavor_t flavor, thread_policy_t policy, mach_msg_type_number_t *cnt, boolean_t *get_default);
