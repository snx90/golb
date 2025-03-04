/* Copyright 2022 0x7ff
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "golb.h"
#include <compression.h>
#include <dlfcn.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#define LZSS_F (18)
#define LZSS_N (4096)
#define LZSS_THRESHOLD (2)
#define KCOMP_HDR_PAD_SZ (0x16C)
#define PROC_P_LIST_LE_PREV_OFF (0x8)
#define VM_MAP_HDR_RBH_ROOT_OFF (0x38)
#define PROC_P_LIST_LH_FIRST_OFF (0x0)
#define LOADED_KEXT_SUMMARY_HDR_NAME_OFF (0x10)
#define LOADED_KEXT_SUMMARY_HDR_ADDR_OFF (0x60)
#if TARGET_OS_OSX
#	define PREBOOT_PATH "/System/Volumes/Preboot"
#else
#	define PREBOOT_PATH "/private/preboot/"
#endif
#define BOOT_PATH "/System/Library/Caches/com.apple.kernelcaches/kernelcache"

#define AP_RWRW (1U)
#define AP_RORO (3U)
#define DER_INT (0x2U)
#define DER_SEQ (0x30U)
#define ARM_PTE_TYPE (3U)
#define PVH_LOCK_BIT (61U)
#define PVH_TYPE_PTEP (2U)
#define ARM_PTE_AF (0x400U)
#define ARM_PTE_NG (0x800U)
#define DER_IA5_STR (0x16U)
#define PVH_TYPE_MASK (3ULL)
#define DER_OCTET_STR (0x4U)
#define ARM_PGSHIFT_4K (12U)
#define ARM_PGSHIFT_16K (14U)
#define PROC_PIDREGIONINFO (7)
#define ARM64_VMADDR_BITS (48U)
#define ARM_PTE_TYPE_VALID (3U)
#define RD(a) extract32(a, 0, 5)
#define RN(a) extract32(a, 5, 5)
#define ARM_PTE_AP(a) ((a) << 6U)
#define VM_KERN_MEMORY_OSKEXT (5)
#define PVH_FLAG_CPU (1ULL << 62U)
#define LOWGLO_VER_CODE "Kraken  "
#define PVH_FLAG_EXEC (1ULL << 60U)
#define CACHE_ATTRINDX_DISABLE (3U)
#define PVH_FLAG_HASHED (1ULL << 58U)
#define KCOMP_HDR_MAGIC (0x636F6D70U)
#define ADRP_ADDR(a) ((a) & ~0xFFFULL)
#define PVH_LIST_MASK (~PVH_TYPE_MASK)
#define VM_MAP_FLAGS_NO_ZERO_FILL (4U)
#define ARM_PGMASK (ARM_PGBYTES - 1ULL)
#define ADRP_IMM(a) (ADR_IMM(a) << 12U)
#define ARM_PGBYTES (1U << arm_pgshift)
#define PVH_FLAG_LOCKDOWN (1ULL << 59U)
#define ARM_PTE_ATTRINDX(a) ((a) << 2U)
#define ARM_PTE_NX (0x40000000000000ULL)
#define ARM_PTE_PNX (0x20000000000000ULL)
#define ADD_X_IMM(a) extract32(a, 10, 12)
#define KCOMP_HDR_TYPE_LZSS (0x6C7A7373U)
#define LOWGLO_LAYOUT_MAGIC (0xC0DEC0DEU)
#define FAULT_MAGIC (0xAAAAAAAAAAAAAAAAULL)
#define PVH_FLAG_LOCK (1ULL << PVH_LOCK_BIT)
#define LDR_X_IMM(a) (sextract64(a, 5, 19) << 2U)
#define kOSBundleLoadAddressKey "OSBundleLoadAddress"
#define IS_ADR(a) (((a) & 0x9F000000U) == 0x10000000U)
#define IS_ADRP(a) (((a) & 0x9F000000U) == 0x90000000U)
#define IS_CBZ_W(a) (((a) & 0xFF000000U) == 0x34000000U)
#define IS_LDR_X(a) (((a) & 0xFF000000U) == 0x58000000U)
#define IS_ADD_X(a) (((a) & 0xFFC00000U) == 0x91000000U)
#define IS_MOV_X(a) (((a) & 0xFFE00000U) == 0xAA000000U)
#define IS_SUBS_X(a) (((a) & 0xFF200000U) == 0xEB000000U)
#define LDR_W_UNSIGNED_IMM(a) (extract32(a, 10, 12) << 2U)
#define LDR_X_UNSIGNED_IMM(a) (extract32(a, 10, 12) << 3U)
#define IS_LDR_X_UXTW_3(a) (((a) & 0xFFE0FC00U) == 0xF8605800U)
#define IS_LDR_W_UNSIGNED_IMM(a) (((a) & 0xFFC00000U) == 0xB9400000U)
#define IS_LDR_X_UNSIGNED_IMM(a) (((a) & 0xFFC00000U) == 0xF9400000U)
#define ADR_IMM(a) ((sextract64(a, 5, 19) << 2U) | extract32(a, 29, 2))
#define ARM_PTE_MASK (((1ULL << ARM64_VMADDR_BITS) - 1U) & ~ARM_PGMASK)
#define PVH_HIGH_FLAGS (PVH_FLAG_CPU | PVH_FLAG_LOCK | PVH_FLAG_EXEC | PVH_FLAG_LOCKDOWN | PVH_FLAG_HASHED | (1ULL << 57U) | (1ULL << 56U) | (1ULL << 55U))

#ifndef SECT_CSTRING
#	define SECT_CSTRING "__cstring"
#endif

#ifndef SEG_TEXT_EXEC
#	define SEG_TEXT_EXEC "__TEXT_EXEC"
#endif

#ifndef MIN
#	define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef kern_return_t (*kernrw_0_kbase_func_t)(kaddr_t *);
typedef int (*krw_0_kbase_func_t)(kaddr_t *), (*krw_0_kread_func_t)(kaddr_t, void *, size_t), (*krw_0_kwrite_func_t)(const void *, kaddr_t, size_t), (*kernrw_0_req_kernrw_func_t)(void);

typedef struct {
	uint16_t rev, ver;
	uint32_t pad;
	kaddr_t virt_base, phys_base;
	uint64_t mem_sz;
} boot_args_t;

typedef struct {
	struct section_64 s64;
	char *data;
} sec_64_t;

typedef struct {
	sec_64_t sec_text, sec_data, sec_cstring;
	const char *kernel;
	size_t kernel_sz;
	kaddr_t pc;
	char *data;
} pfinder_t;

typedef struct {
	struct {
		uint32_t next, prev;
	} vmp_q_pageq, vmp_listq, vmp_backgroundq;
	uint64_t vmp_offset;
	uint32_t vmp_object, q_flags, vmp_next_m, o_flags;
} vm_page_t;

typedef struct {
	struct {
		kaddr_t prev, next, start, end;
	} links;
	kaddr_t rbe_left, rbe_right, rbe_parent, vme_object;
	uint64_t vme_offset;
} vm_map_entry_t;

typedef struct {
	uint8_t ver_code[8];
	kaddr_t zero, stext, ver, os_ver, kmod_ptr, trans_off, reboot_flag, manual_pkt_addr, alt_debugger, pmap_memq, pmap_mem_page_off, pmap_mem_chain_off, static_addr, static_sz, layout_major_ver, layout_magic, pmap_mem_start_addr, pmap_mem_end_addr, pmap_mem_page_sz, pmap_mem_from_array_mask, pmap_mem_first_ppnum, pmap_mem_packed_shift, pmap_mem_packed_base_addr, layout_minor_ver, page_shift;
} lowglo_t;

static lowglo_t lowglo;
static int kmem_fd = -1;
static boot_args_t boot_args;
static void *krw_0, *kernrw_0;
static kread_func_t kread_buf;
static task_t tfp0 = TASK_NULL;
static uint64_t proc_struct_sz;
static kwrite_func_t kwrite_buf;
static krw_0_kread_func_t krw_0_kread;
static unsigned t1sz_boot, arm_pgshift;
static krw_0_kwrite_func_t krw_0_kwrite;
static bool has_proc_struct_sz, has_vm_obj_packed_ptr;
static size_t task_map_off, proc_task_off, proc_p_pid_off, vm_map_pmap_off, pmap_sw_asid_off, vm_map_flags_off;
static kaddr_t kbase, kernproc, pv_head_table_ptr, const_boot_args, lowglo_ptr, pv_head_table, proc_struct_sz_ptr, vm_kernel_link_addr, our_map, our_pmap;

static uint32_t
extract32(uint32_t val, unsigned start, unsigned len) {
	return (val >> start) & (~0U >> (32U - len));
}

static uint64_t
sextract64(uint64_t val, unsigned start, unsigned len) {
	return (uint64_t)((int64_t)(val << (64U - len - start)) >> (64U - len));
}

static void
kxpacd(kaddr_t *addr) {
	if(t1sz_boot != 0) {
		*addr |= ~((1ULL << (64U - t1sz_boot)) - 1U);
	}
}

static size_t
decompress_lzss(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
	const uint8_t *src_end = src + src_len, *dst_start = dst, *dst_end = dst + dst_len;
	uint16_t i, r = LZSS_N - LZSS_F, flags = 0;
	uint8_t text_buf[LZSS_N + LZSS_F - 1], j;

	memset(text_buf, ' ', r);
	while(src != src_end && dst != dst_end) {
		if(((flags >>= 1U) & 0x100U) == 0) {
			flags = *src++ | 0xFF00U;
			if(src == src_end) {
				break;
			}
		}
		if((flags & 1U) != 0) {
			text_buf[r++] = *dst++ = *src++;
			r &= LZSS_N - 1U;
		} else {
			i = *src++;
			if(src == src_end) {
				break;
			}
			j = *src++;
			i |= (j & 0xF0U) << 4U;
			j = (j & 0xFU) + LZSS_THRESHOLD;
			do {
				*dst++ = text_buf[r++] = text_buf[i++ & (LZSS_N - 1U)];
				r &= LZSS_N - 1U;
			} while(j-- != 0 && dst != dst_end);
		}
	}
	return (size_t)(dst - dst_start);
}

static const uint8_t *
der_decode(uint8_t tag, const uint8_t *der, const uint8_t *der_end, size_t *out_len) {
	size_t der_len;

	if(der_end - der > 2 && tag == *der++) {
		if(((der_len = *der++) & 0x80U) != 0) {
			*out_len = 0;
			if((der_len &= 0x7FU) <= sizeof(*out_len) && (size_t)(der_end - der) >= der_len) {
				while(der_len-- != 0) {
					*out_len = (*out_len << 8U) | *der++;
				}
			}
		} else {
			*out_len = der_len;
		}
		if(*out_len != 0 && (size_t)(der_end - der) >= *out_len) {
			return der;
		}
	}
	return NULL;
}

static const uint8_t *
der_decode_seq(const uint8_t *der, const uint8_t *der_end, const uint8_t **seq_end) {
	size_t der_len;

	if((der = der_decode(DER_SEQ, der, der_end, &der_len)) != NULL) {
		*seq_end = der + der_len;
	}
	return der;
}

static const uint8_t *
der_decode_uint64(const uint8_t *der, const uint8_t *der_end, uint64_t *r) {
	size_t der_len;

	if((der = der_decode(DER_INT, der, der_end, &der_len)) != NULL && (*der & 0x80U) == 0 && (der_len <= sizeof(*r) || (--der_len == sizeof(*r) && *der++ == 0))) {
		*r = 0;
		while(der_len-- != 0) {
			*r = (*r << 8U) | *der++;
		}
		return der;
	}
	return NULL;
}

static void *
kdecompress(const void *src, size_t src_len, size_t *dst_len) {
	const uint8_t *der, *octet, *der_end, *src_end = (const uint8_t *)src + src_len;
	struct {
		uint32_t magic, type, adler32, uncomp_sz, comp_sz;
		uint8_t pad[KCOMP_HDR_PAD_SZ];
	} kcomp_hdr;
	size_t der_len;
	uint64_t r;
	void *dst;

	if((der = der_decode_seq(src, src_end, &der_end)) != NULL && (der = der_decode(DER_IA5_STR, der, der_end, &der_len)) != NULL && der_len == 4 && (memcmp(der, "IMG4", der_len) != 0 || ((der = der_decode_seq(der + der_len, src_end, &der_end)) != NULL && (der = der_decode(DER_IA5_STR, der, der_end, &der_len)) != NULL && der_len == 4)) && memcmp(der, "IM4P", der_len) == 0 && (der = der_decode(DER_IA5_STR, der + der_len, der_end, &der_len)) != NULL && der_len == 4 && memcmp(der, "krnl", der_len) == 0 && (der = der_decode(DER_IA5_STR, der + der_len, der_end, &der_len)) != NULL && (der = der_decode(DER_OCTET_STR, der + der_len, der_end, &der_len)) != NULL && der_len > sizeof(kcomp_hdr)) {
		octet = der;
		memcpy(&kcomp_hdr, octet, sizeof(kcomp_hdr));
		if(kcomp_hdr.magic == __builtin_bswap32(KCOMP_HDR_MAGIC)) {
			if(kcomp_hdr.type == __builtin_bswap32(KCOMP_HDR_TYPE_LZSS) && (kcomp_hdr.comp_sz = __builtin_bswap32(kcomp_hdr.comp_sz)) <= der_len - sizeof(kcomp_hdr) && (kcomp_hdr.uncomp_sz = __builtin_bswap32(kcomp_hdr.uncomp_sz)) != 0 && (dst = malloc(kcomp_hdr.uncomp_sz)) != NULL) {
				if(decompress_lzss(octet + sizeof(kcomp_hdr), kcomp_hdr.comp_sz, dst, kcomp_hdr.uncomp_sz) == kcomp_hdr.uncomp_sz) {
					*dst_len = kcomp_hdr.uncomp_sz;
					return dst;
				}
				free(dst);
			}
		} else if((der = der_decode_seq(der + der_len, src_end, &der_end)) != NULL && (der = der_decode_uint64(der, der_end, &r)) != NULL && r == 1 && der_decode_uint64(der, der_end, &r) != NULL && r != 0 && (dst = malloc(r)) != NULL) {
			if(compression_decode_buffer(dst, r, octet, der_len, NULL, COMPRESSION_LZFSE) == r) {
				*dst_len = r;
				return dst;
			}
			free(dst);
		}
	}
	return NULL;
}

static kern_return_t
init_arm_pgshift(void) {
	uint32_t cpufamily = CPUFAMILY_UNKNOWN;
	size_t len = sizeof(cpufamily);

	if(sysctlbyname("hw.cpufamily", &cpufamily, &len, NULL, 0) == 0) {
		switch(cpufamily) {
			case 0x37A09642U: /* CPUFAMILY_ARM_CYCLONE */
			case 0x2C91A47EU: /* CPUFAMILY_ARM_TYPHOON */
				arm_pgshift = ARM_PGSHIFT_4K;
				return KERN_SUCCESS;
			case 0x92FB37C8U: /* CPUFAMILY_ARM_TWISTER */
			case 0x67CEEE93U: /* CPUFAMILY_ARM_HURRICANE */
			case 0xE81E7EF6U: /* CPUFAMILY_ARM_MONSOON_MISTRAL */
				arm_pgshift = ARM_PGSHIFT_16K;
				return KERN_SUCCESS;
			default:
				break;
		}
	}
	return KERN_FAILURE;
}

static kern_return_t
kread_buf_krw_0(kaddr_t addr, void *buf, size_t sz) {
	return krw_0_kread(addr, buf, sz) == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

static kern_return_t
kwrite_buf_krw_0(kaddr_t addr, const void *buf, size_t sz) {
	return krw_0_kwrite(buf, addr, sz) == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

static kern_return_t
init_tfp0(void) {
	kern_return_t ret = task_for_pid(mach_task_self(), 0, &tfp0);
	mach_port_t host;
	pid_t pid;

	if(ret != KERN_SUCCESS) {
		host = mach_host_self();
		if(MACH_PORT_VALID(host)) {
			printf("host: 0x%" PRIX32 "\n", host);
			ret = host_get_special_port(host, HOST_LOCAL_NODE, 4, &tfp0);
			mach_port_deallocate(mach_task_self(), host);
		}
	}
	if(ret == KERN_SUCCESS && MACH_PORT_VALID(tfp0)) {
		if(pid_for_task(tfp0, &pid) == KERN_SUCCESS) {
			return ret;
		}
		mach_port_deallocate(mach_task_self(), tfp0);
	}
	return KERN_FAILURE;
}

static kern_return_t
kread_buf_tfp0(kaddr_t addr, void *buf, size_t sz) {
	mach_vm_address_t p = (mach_vm_address_t)buf;
	mach_vm_size_t read_sz, out_sz = 0;

	while(sz != 0) {
		read_sz = MIN(sz, vm_kernel_page_size - (addr & vm_kernel_page_mask));
		if(mach_vm_read_overwrite(tfp0, addr, read_sz, p, &out_sz) != KERN_SUCCESS || out_sz != read_sz) {
			return KERN_FAILURE;
		}
		p += read_sz;
		sz -= read_sz;
		addr += read_sz;
	}
	return KERN_SUCCESS;
}

static kern_return_t
kread_addr(kaddr_t addr, kaddr_t *val) {
	return kread_buf(addr, val, sizeof(*val));
}

static kern_return_t
kwrite_buf_tfp0(kaddr_t addr, const void *buf, size_t sz) {
	vm_machine_attribute_val_t mattr_val = MATTR_VAL_CACHE_FLUSH;
	mach_vm_address_t p = (mach_vm_address_t)buf;
	mach_msg_type_number_t write_sz;

	while(sz != 0) {
		write_sz = (mach_msg_type_number_t)MIN(sz, vm_kernel_page_size - (addr & vm_kernel_page_mask));
		if(mach_vm_write(tfp0, addr, p, write_sz) != KERN_SUCCESS || mach_vm_machine_attribute(tfp0, addr, write_sz, MATTR_CACHE, &mattr_val) != KERN_SUCCESS) {
			return KERN_FAILURE;
		}
		p += write_sz;
		sz -= write_sz;
		addr += write_sz;
	}
	return KERN_SUCCESS;
}

static kern_return_t
kread_buf_kmem(kaddr_t addr, void *buf, size_t sz) {
	mach_vm_size_t read_sz;
	char *p = buf;
	ssize_t n;

	while(sz != 0) {
		read_sz = (mach_vm_size_t)MIN(sz, vm_kernel_page_size - (addr & vm_kernel_page_mask));
		if((n = pread(kmem_fd, p, read_sz, (off_t)addr)) < 0 || (size_t)n != read_sz) {
			return KERN_FAILURE;
		}
		p += read_sz;
		sz -= read_sz;
		addr += read_sz;
	}
	return KERN_SUCCESS;
}

static kern_return_t
kwrite_buf_kmem(kaddr_t addr, const void *buf, size_t sz) {
	mach_msg_type_number_t write_sz;
	const char *p = buf;
	ssize_t n;

	while(sz != 0) {
		write_sz = (mach_msg_type_number_t)MIN(sz, vm_kernel_page_size - (addr & vm_kernel_page_mask));
		if((n = pwrite(kmem_fd, p, write_sz, (off_t)addr)) < 0 || (size_t)n != write_sz) {
			return KERN_FAILURE;
		}
		p += write_sz;
		sz -= write_sz;
		addr += write_sz;
	}
	return KERN_SUCCESS;
}

static kern_return_t
kwrite_addr(kaddr_t addr, kaddr_t val) {
	return kwrite_buf(addr, &val, sizeof(val));
}

static kern_return_t
find_section_kernel(kaddr_t p, struct segment_command_64 sg64, const char *sect_name, struct section_64 *sp) {
	for(; sg64.nsects-- != 0; p += sizeof(*sp)) {
		if(kread_buf(p, sp, sizeof(*sp)) != KERN_SUCCESS) {
			break;
		}
		if((sp->flags & SECTION_TYPE) != S_ZEROFILL) {
			if(sp->offset < sg64.fileoff || sp->size > sg64.filesize || sp->offset - sg64.fileoff > sg64.filesize - sp->size) {
				break;
			}
			if(sp->size != 0 && strncmp(sp->segname, sg64.segname, sizeof(sp->segname)) == 0 && strncmp(sp->sectname, sect_name, sizeof(sp->sectname)) == 0) {
				return KERN_SUCCESS;
			}
		}
	}
	return KERN_FAILURE;
}

static kern_return_t
find_section_macho(const char *p, struct segment_command_64 sg64, const char *sect_name, struct section_64 *sp) {
	for(; sg64.nsects-- != 0; p += sizeof(*sp)) {
		memcpy(sp, p, sizeof(*sp));
		if((sp->flags & SECTION_TYPE) != S_ZEROFILL) {
			if(sp->offset < sg64.fileoff || sp->size > sg64.filesize || sp->offset - sg64.fileoff > sg64.filesize - sp->size) {
				break;
			}
			if(sp->size != 0 && strncmp(sp->segname, sg64.segname, sizeof(sp->segname)) == 0 && strncmp(sp->sectname, sect_name, sizeof(sp->sectname)) == 0) {
				return KERN_SUCCESS;
			}
		}
	}
	return KERN_FAILURE;
}

static void
sec_reset(sec_64_t *sec) {
	memset(&sec->s64, '\0', sizeof(sec->s64));
	sec->data = NULL;
}

static void
sec_term(sec_64_t *sec) {
	free(sec->data);
}

static kern_return_t
sec_read_buf(sec_64_t sec, kaddr_t addr, void *buf, size_t sz) {
	size_t off;

	if(addr < sec.s64.addr || sz > sec.s64.size || (off = addr - sec.s64.addr) > sec.s64.size - sz) {
		return KERN_FAILURE;
	}
	memcpy(buf, sec.data + off, sz);
	return KERN_SUCCESS;
}

static void
pfinder_reset(pfinder_t *pfinder) {
	pfinder->pc = 0;
	pfinder->data = NULL;
	pfinder->kernel = NULL;
	pfinder->kernel_sz = 0;
	sec_reset(&pfinder->sec_text);
	sec_reset(&pfinder->sec_data);
	sec_reset(&pfinder->sec_cstring);
}

static void
pfinder_term(pfinder_t *pfinder) {
	free(pfinder->data);
	sec_term(&pfinder->sec_text);
	sec_term(&pfinder->sec_data);
	sec_term(&pfinder->sec_cstring);
	pfinder_reset(pfinder);
}

static kern_return_t
pfinder_init_macho(pfinder_t *pfinder, size_t off) {
	struct {
		uint32_t flavor, cnt;
		kaddr_t x[29], fp, lr, sp, pc;
		uint32_t cpsr, pad;
	} state;
	const char *p = pfinder->kernel + off, *e;
#if TARGET_OS_OSX
	struct fileset_entry_command fec;
#endif
	struct segment_command_64 sg64;
	struct mach_header_64 mh64;
	struct load_command lc;
	struct section_64 s64;

	memcpy(&mh64, p, sizeof(mh64));
	if(mh64.magic == MH_MAGIC_64 && mh64.cputype == CPU_TYPE_ARM64 &&
#if TARGET_OS_OSX
	   (mh64.filetype == MH_EXECUTE || (off == 0 && mh64.filetype == MH_FILESET))
#else
	   mh64.filetype == MH_EXECUTE
#endif
	   && mh64.sizeofcmds < (pfinder->kernel_sz - sizeof(mh64)) - off) {
		for(p += sizeof(mh64), e = p + mh64.sizeofcmds; mh64.ncmds-- != 0 && (size_t)(e - p) >= sizeof(lc); p += lc.cmdsize) {
			memcpy(&lc, p, sizeof(lc));
			if(lc.cmdsize < sizeof(lc) || (size_t)(e - p) < lc.cmdsize) {
				break;
			}
			if(lc.cmd == LC_SEGMENT_64) {
				if(lc.cmdsize < sizeof(sg64)) {
					break;
				}
				memcpy(&sg64, p, sizeof(sg64));
				if(sg64.vmsize == 0) {
					continue;
				}
				if(sg64.nsects != (lc.cmdsize - sizeof(sg64)) / sizeof(s64) || sg64.fileoff > pfinder->kernel_sz || sg64.filesize > pfinder->kernel_sz - sg64.fileoff) {
					break;
				}
				if(mh64.filetype == MH_EXECUTE) {
					if(strncmp(sg64.segname, SEG_TEXT_EXEC, sizeof(sg64.segname)) == 0) {
						if(find_section_macho(p + sizeof(sg64), sg64, SECT_TEXT, &s64) != KERN_SUCCESS || s64.size == 0 || (pfinder->sec_text.data = malloc(s64.size)) == NULL) {
							break;
						}
						memcpy(pfinder->sec_text.data, pfinder->kernel + s64.offset, s64.size);
						pfinder->sec_text.s64 = s64;
						printf("sec_text_addr: " KADDR_FMT ", sec_text_off: 0x%" PRIX32 ", sec_text_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					} else if(strncmp(sg64.segname, SEG_DATA, sizeof(sg64.segname)) == 0) {
						if(find_section_macho(p + sizeof(sg64), sg64, SECT_DATA, &s64) != KERN_SUCCESS || s64.size == 0 || (pfinder->sec_data.data = malloc(s64.size)) == NULL) {
							break;
						}
						memcpy(pfinder->sec_data.data, pfinder->kernel + s64.offset, s64.size);
						pfinder->sec_data.s64 = s64;
						printf("sec_data_addr: " KADDR_FMT ", sec_data_off: 0x%" PRIX32 ", sec_data_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					} else if(strncmp(sg64.segname, SEG_TEXT, sizeof(sg64.segname)) == 0) {
						if(find_section_macho(p + sizeof(sg64), sg64, SECT_CSTRING, &s64) != KERN_SUCCESS || s64.size == 0 || (pfinder->sec_cstring.data = calloc(1, s64.size + 1)) == NULL) {
							break;
						}
						memcpy(pfinder->sec_cstring.data, pfinder->kernel + s64.offset, s64.size);
						pfinder->sec_cstring.s64 = s64;
						printf("sec_cstring_addr: " KADDR_FMT ", sec_cstring_off: 0x%" PRIX32 ", sec_cstring_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					}
				}
			} else if(lc.cmd == LC_UNIXTHREAD) {
				if(lc.cmdsize != sizeof(struct thread_command) + sizeof(state)) {
					break;
				}
				memcpy(&state, p + sizeof(struct thread_command), sizeof(state));
				pfinder->pc = state.pc;
				printf("pc: " KADDR_FMT "\n", state.pc);
			}
#if TARGET_OS_OSX
			else if(mh64.filetype == MH_FILESET && lc.cmd == LC_FILESET_ENTRY) {
				if(lc.cmdsize < sizeof(fec)) {
					break;
				}
				memcpy(&fec, p, sizeof(fec));
				if(fec.fileoff == 0 || fec.fileoff > pfinder->kernel_sz - sizeof(mh64) || fec.entry_id.offset > fec.cmdsize || p[fec.cmdsize - 1] != '\0') {
					break;
				}
				if(strcmp(p + fec.entry_id.offset, "com.apple.kernel") == 0 && pfinder_init_macho(pfinder, fec.fileoff) == KERN_SUCCESS) {
					return KERN_SUCCESS;
				}
			}
#endif
			if(pfinder->pc != 0 && pfinder->sec_text.s64.size != 0 && pfinder->sec_data.s64.size != 0 && pfinder->sec_cstring.s64.size != 0) {
				pfinder->sec_text.s64.addr += kbase - vm_kernel_link_addr;
				pfinder->sec_data.s64.addr += kbase - vm_kernel_link_addr;
				pfinder->sec_cstring.s64.addr += kbase - vm_kernel_link_addr;
				return KERN_SUCCESS;
			}
		}
	}
	return KERN_FAILURE;
}

#if TARGET_OS_OSX
static int
kstrcmp(kaddr_t p, const char *s0) {
	size_t len = strlen(s0);
	int ret = 1;
	char *s;

	if((s = malloc(len + 1)) != NULL) {
		s[len] = '\0';
		if(kread_buf(p, s, len) == KERN_SUCCESS) {
			ret = strcmp(s, s0);
		}
		free(s);
	}
	return ret;
}
#endif

static kern_return_t
pfinder_init_kernel(pfinder_t *pfinder, size_t off) {
	struct {
		uint32_t flavor, cnt;
		kaddr_t x[29], fp, lr, sp, pc;
		uint32_t cpsr, pad;
	} state;
#if TARGET_OS_OSX
	struct fileset_entry_command fec;
#endif
	struct segment_command_64 sg64;
	kaddr_t p = kbase + off, e;
	struct mach_header_64 mh64;
	struct load_command lc;
	struct section_64 s64;

	if(kread_buf(p, &mh64, sizeof(mh64)) == KERN_SUCCESS && mh64.magic == MH_MAGIC_64 && mh64.cputype == CPU_TYPE_ARM64 &&
#if TARGET_OS_OSX
	   (mh64.filetype == MH_EXECUTE || (off == 0 && mh64.filetype == MH_FILESET))
#else
	   mh64.filetype == MH_EXECUTE
#endif
	   ) {
		for(p += sizeof(mh64), e = p + mh64.sizeofcmds; mh64.ncmds-- != 0 && e - p >= sizeof(lc); p += lc.cmdsize) {
			if(kread_buf(p, &lc, sizeof(lc)) != KERN_SUCCESS || lc.cmdsize < sizeof(lc) || e - p < lc.cmdsize) {
				break;
			}
			if(lc.cmd == LC_SEGMENT_64) {
				if(lc.cmdsize < sizeof(sg64) || kread_buf(p, &sg64, sizeof(sg64)) != KERN_SUCCESS) {
					break;
				}
				if(sg64.vmsize == 0) {
					continue;
				}
				if(sg64.nsects != (lc.cmdsize - sizeof(sg64)) / sizeof(s64)) {
					break;
				}
				if(mh64.filetype == MH_EXECUTE) {
					if(strncmp(sg64.segname, SEG_TEXT_EXEC, sizeof(sg64.segname)) == 0) {
						if(find_section_kernel(p + sizeof(sg64), sg64, SECT_TEXT, &s64) != KERN_SUCCESS || s64.size == 0 || (pfinder->sec_text.data = malloc(s64.size)) == NULL || kread_buf(s64.addr, pfinder->sec_text.data, s64.size) != KERN_SUCCESS) {
							break;
						}
						pfinder->sec_text.s64 = s64;
						printf("sec_text_addr: " KADDR_FMT ", sec_text_off: 0x%" PRIX32 ", sec_text_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					} else if(strncmp(sg64.segname, SEG_DATA, sizeof(sg64.segname)) == 0) {
						if(find_section_kernel(p + sizeof(sg64), sg64, SECT_DATA, &s64) != KERN_SUCCESS || s64.size == 0 || (pfinder->sec_data.data = malloc(s64.size)) == NULL || kread_buf(s64.addr, pfinder->sec_data.data, s64.size) != KERN_SUCCESS) {
							break;
						}
						pfinder->sec_data.s64 = s64;
						printf("sec_data_addr: " KADDR_FMT ", sec_data_off: 0x%" PRIX32 ", sec_data_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					} else if(strncmp(sg64.segname, SEG_TEXT, sizeof(sg64.segname)) == 0) {
						if(find_section_kernel(p + sizeof(sg64), sg64, SECT_CSTRING, &s64) != KERN_SUCCESS || s64.size == 0 || (pfinder->sec_cstring.data = calloc(1, s64.size + 1)) == NULL || kread_buf(s64.addr, pfinder->sec_cstring.data, s64.size) != KERN_SUCCESS) {
							break;
						}
						pfinder->sec_cstring.s64 = s64;
						printf("sec_cstring_addr: " KADDR_FMT ", sec_cstring_off: 0x%" PRIX32 ", sec_cstring_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					}
				}
			} else if(lc.cmd == LC_UNIXTHREAD) {
				if(lc.cmdsize != sizeof(struct thread_command) + sizeof(state) || kread_buf(p + sizeof(struct thread_command), &state, sizeof(state)) != KERN_SUCCESS) {
					break;
				}
				pfinder->pc = state.pc;
				printf("pc: " KADDR_FMT "\n", state.pc);
			}
#if TARGET_OS_OSX
			else if(mh64.filetype == MH_FILESET && lc.cmd == LC_FILESET_ENTRY) {
				if(lc.cmdsize < sizeof(fec) || kread_buf(p, &fec, sizeof(fec)) != KERN_SUCCESS) {
					break;
				}
				if(fec.fileoff == 0 || fec.entry_id.offset > fec.cmdsize) {
					break;
				}
				if(kstrcmp(p + fec.entry_id.offset, "com.apple.kernel") == 0 && pfinder_init_kernel(pfinder, fec.fileoff) == KERN_SUCCESS) {
					return KERN_SUCCESS;
				}
			}
#endif
			if(pfinder->pc != 0 && pfinder->sec_text.s64.size != 0 && pfinder->sec_data.s64.size != 0 && pfinder->sec_cstring.s64.size != 0) {
				return KERN_SUCCESS;
			}
		}
	}
	return KERN_FAILURE;
}

static kern_return_t
pfinder_init_file(pfinder_t *pfinder, const char *filename) {
	kern_return_t ret = KERN_FAILURE;
	struct mach_header_64 mh64;
	struct fat_header fh;
	struct stat stat_buf;
	struct fat_arch fa;
	const char *p;
	size_t len;
	void *m;
	int fd;

	pfinder_reset(pfinder);
	if((fd = open(filename, O_RDONLY | O_CLOEXEC)) != -1) {
		if(fstat(fd, &stat_buf) != -1 && S_ISREG(stat_buf.st_mode) && stat_buf.st_size > 0) {
			len = (size_t)stat_buf.st_size;
			if((m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0)) != MAP_FAILED) {
				if((pfinder->data = kdecompress(m, len, &pfinder->kernel_sz)) != NULL && pfinder->kernel_sz > sizeof(fh) + sizeof(mh64)) {
					pfinder->kernel = pfinder->data;
					memcpy(&fh, pfinder->kernel, sizeof(fh));
					if(fh.magic == __builtin_bswap32(FAT_MAGIC) && (fh.nfat_arch = __builtin_bswap32(fh.nfat_arch)) < (pfinder->kernel_sz - sizeof(fh)) / sizeof(fa)) {
						for(p = pfinder->kernel + sizeof(fh); fh.nfat_arch-- != 0; p += sizeof(fa)) {
							memcpy(&fa, p, sizeof(fa));
							if(fa.cputype == (cpu_type_t)__builtin_bswap32(CPU_TYPE_ARM64) && (fa.offset = __builtin_bswap32(fa.offset)) < pfinder->kernel_sz && (fa.size = __builtin_bswap32(fa.size)) <= pfinder->kernel_sz - fa.offset && fa.size > sizeof(mh64)) {
								pfinder->kernel_sz = fa.size;
								pfinder->kernel += fa.offset;
								break;
							}
						}
					}
					ret = pfinder_init_macho(pfinder, 0);
				}
				munmap(m, len);
			}
		}
		close(fd);
	}
	if(ret != KERN_SUCCESS) {
		pfinder_term(pfinder);
	}
	return ret;
}

static char *
get_boot_path(void) {
	size_t path_len = sizeof(BOOT_PATH);
#if TARGET_OS_OSX
	CFDataRef boot_objects_path_cf;
	size_t boot_objects_path_len;
#else
	const uint8_t *hash;
	CFDataRef hash_cf;
	size_t hash_len;
#endif
	io_registry_entry_t chosen;
	struct stat stat_buf;
	char *path = NULL;

	if(stat(PREBOOT_PATH, &stat_buf) != -1 && S_ISDIR(stat_buf.st_mode)) {
		if((chosen = IORegistryEntryFromPath(kIOMasterPortDefault, kIODeviceTreePlane ":/chosen")) != IO_OBJECT_NULL) {
			path_len += strlen(PREBOOT_PATH);
#if TARGET_OS_OSX
			if((boot_objects_path_cf = IORegistryEntryCreateCFProperty(chosen, CFSTR("boot-objects-path"), kCFAllocatorDefault, kNilOptions)) != NULL) {
				if(CFGetTypeID(boot_objects_path_cf) == CFDataGetTypeID() && (boot_objects_path_len = (size_t)CFDataGetLength(boot_objects_path_cf) - 1) != 0) {
					path_len += boot_objects_path_len;
					if((path = malloc(path_len)) != NULL) {
						memcpy(path, PREBOOT_PATH, strlen(PREBOOT_PATH));
						memcpy(path + strlen(PREBOOT_PATH), CFDataGetBytePtr(boot_objects_path_cf), boot_objects_path_len);
					}
				}
				CFRelease(boot_objects_path_cf);
			}
#else
			if((hash_cf = IORegistryEntryCreateCFProperty(chosen, CFSTR("boot-manifest-hash"), kCFAllocatorDefault, kNilOptions)) != NULL) {
				if(CFGetTypeID(hash_cf) == CFDataGetTypeID() && (hash_len = (size_t)CFDataGetLength(hash_cf) << 1U) != 0) {
					path_len += hash_len;
					if((path = malloc(path_len)) != NULL) {
						memcpy(path, PREBOOT_PATH, strlen(PREBOOT_PATH));
						for(hash = CFDataGetBytePtr(hash_cf); hash_len-- != 0; ) {
							path[strlen(PREBOOT_PATH) + hash_len] = "0123456789ABCDEF"[(hash[hash_len >> 1U] >> ((~hash_len & 1U) << 2U)) & 0xFU];
						}
					}
				}
				CFRelease(hash_cf);
			}
#endif
			IOObjectRelease(chosen);
		}
	} else if(stat(BOOT_PATH, &stat_buf) != -1 && S_ISREG(stat_buf.st_mode)) {
		path = malloc(path_len);
	}
	if(path != NULL) {
		memcpy(path + (path_len - sizeof(BOOT_PATH)), BOOT_PATH, sizeof(BOOT_PATH));
	}
	return path;
}

static kern_return_t
pfinder_init(pfinder_t *pfinder) {
	char *boot_path = get_boot_path();
	kern_return_t ret = KERN_FAILURE;

	pfinder_reset(pfinder);
	if(boot_path != NULL) {
		printf("boot_path: %s\n", boot_path);
		if((ret = pfinder_init_file(pfinder, boot_path)) != KERN_SUCCESS && (ret = pfinder_init_kernel(pfinder, 0)) != KERN_SUCCESS) {
			pfinder_term(pfinder);
		}
		free(boot_path);
	} else if((ret = pfinder_init_kernel(pfinder, 0)) != KERN_SUCCESS) {
		pfinder_term(pfinder);
	}
	return ret;
}

static kaddr_t
pfinder_xref_rd(pfinder_t pfinder, uint32_t rd, kaddr_t start, kaddr_t to) {
	kaddr_t x[32] = { 0 };
	uint32_t insn;

	for(; sec_read_buf(pfinder.sec_text, start, &insn, sizeof(insn)) == KERN_SUCCESS; start += sizeof(insn)) {
		if(IS_LDR_X(insn)) {
			x[RD(insn)] = start + LDR_X_IMM(insn);
		} else if(IS_ADR(insn)) {
			x[RD(insn)] = start + ADR_IMM(insn);
		} else if(IS_ADD_X(insn)) {
			x[RD(insn)] = x[RN(insn)] + ADD_X_IMM(insn);
		} else if(IS_LDR_W_UNSIGNED_IMM(insn)) {
			x[RD(insn)] = x[RN(insn)] + LDR_W_UNSIGNED_IMM(insn);
		} else if(IS_LDR_X_UNSIGNED_IMM(insn)) {
			x[RD(insn)] = x[RN(insn)] + LDR_X_UNSIGNED_IMM(insn);
		} else {
			if(IS_ADRP(insn)) {
				x[RD(insn)] = ADRP_ADDR(start) + ADRP_IMM(insn);
			}
			continue;
		}
		if(RD(insn) == rd) {
			if(to == 0) {
				return x[rd];
			}
			if(x[rd] == to) {
				return start;
			}
		}
	}
	return 0;
}

static kaddr_t
pfinder_xref_str(pfinder_t pfinder, const char *str, uint32_t rd) {
	const char *p, *e;
	size_t len;

	for(p = pfinder.sec_cstring.data, e = p + pfinder.sec_cstring.s64.size; p != e; p += len) {
		len = strlen(p) + 1;
		if(strncmp(str, p, len) == 0) {
			return pfinder_xref_rd(pfinder, rd, pfinder.sec_text.s64.addr, pfinder.sec_cstring.s64.addr + (kaddr_t)(p - pfinder.sec_cstring.data));
		}
	}
	return 0;
}

static kaddr_t
pfinder_kernproc(pfinder_t pfinder) {
	kaddr_t ref = pfinder_xref_str(pfinder, "Should never have an EVFILT_READ except for reg or fifo. @%s:%d", 0);
	uint32_t insns[2];

	if(ref == 0) {
		ref = pfinder_xref_str(pfinder, "\"Should never have an EVFILT_READ except for reg or fifo.\"", 0);
	}
	for(; sec_read_buf(pfinder.sec_text, ref, insns, sizeof(insns)) == KERN_SUCCESS; ref -= sizeof(*insns)) {
		if(IS_ADRP(insns[0]) && IS_LDR_X_UNSIGNED_IMM(insns[1]) && RD(insns[1]) == 3) {
			return pfinder_xref_rd(pfinder, RD(insns[1]), ref, 0);
		}
	}
	return 0;
}

static kaddr_t
pfinder_pv_head_table_ptr(pfinder_t pfinder) {
	kaddr_t ref = pfinder_xref_str(pfinder, "pmap_init_pte_page(): invalid PVH type for pte_p %p @%s:%d", 0);
	uint32_t insns[3];

	if(ref != 0) {
		for(; sec_read_buf(pfinder.sec_text, ref, insns, sizeof(insns)) == KERN_SUCCESS; ref -= sizeof(*insns)) {
			if(IS_ADRP(insns[0]) && IS_LDR_X_UNSIGNED_IMM(insns[1]) && IS_LDR_X_UXTW_3(insns[2])) {
				return pfinder_xref_rd(pfinder, RD(insns[1]), ref, 0);
			}
		}
	} else if((ref = pfinder_xref_str(pfinder, "\"pmap_init_pte_page(): invalid PVH type for pte_p %p\"", 0)) != 0) {
		for(; sec_read_buf(pfinder.sec_text, ref, insns, sizeof(insns)) == KERN_SUCCESS; ref -= sizeof(*insns)) {
			if(IS_ADRP(insns[0]) && IS_LDR_X_UNSIGNED_IMM(insns[1]) && IS_MOV_X(insns[2]) && RD(insns[2]) == 0) {
				return pfinder_xref_rd(pfinder, RD(insns[1]), ref, 0);
			}
		}
	} else {
		for(ref = pfinder_xref_str(pfinder, "\"pmap_batch_set_cache_attributes(): pn 0x%08x not managed\\n\"", 0); sec_read_buf(pfinder.sec_text, ref, insns, sizeof(insns)) == KERN_SUCCESS; ref += sizeof(*insns)) {
			if(IS_CBZ_W(insns[0]) && IS_ADRP(insns[1]) && IS_LDR_X_UNSIGNED_IMM(insns[2])) {
				return pfinder_xref_rd(pfinder, RD(insns[2]), ref + sizeof(*insns), 0);
			}
		}
	}
	return 0;
}

static kaddr_t
pfinder_const_boot_args(pfinder_t pfinder) {
	return pfinder_xref_rd(pfinder, 20, ADRP_ADDR(kbase + (pfinder.pc - vm_kernel_link_addr)), 0);
}

static kaddr_t
pfinder_lowglo_ptr(pfinder_t pfinder) {
	kaddr_t ref;

	for(ref = pfinder.sec_data.s64.addr; sec_read_buf(pfinder.sec_data, ref, &lowglo, sizeof(lowglo)) == KERN_SUCCESS; ref += PAGE_MAX_SIZE) {
		if(memcmp(&lowglo.ver_code, LOWGLO_VER_CODE, sizeof(lowglo.ver_code)) == 0 && lowglo.layout_magic == LOWGLO_LAYOUT_MAGIC && lowglo.pmap_mem_page_sz == sizeof(vm_page_t)) {
			return ref;
		}
	}
	return 0;
}

static kaddr_t
pfinder_proc_struct_sz_ptr(pfinder_t pfinder) {
	uint32_t insns[3];
	kaddr_t ref;

	for(ref = pfinder_xref_str(pfinder, "panic: ticket lock acquired check done outside of kernel debugger @%s:%d", 0); sec_read_buf(pfinder.sec_text, ref, insns, sizeof(insns)) == KERN_SUCCESS; ref -= sizeof(*insns)) {
		if(IS_ADRP(insns[0]) && IS_LDR_X_UNSIGNED_IMM(insns[1]) && IS_SUBS_X(insns[2]) && RD(insns[2]) == 1) {
			return pfinder_xref_rd(pfinder, RD(insns[1]), ref, 0);
		}
	}
	return 0;
}

static kern_return_t
init_kbase(void) {
	struct {
		uint32_t pri_prot, pri_max_prot, pri_inheritance, pri_flags;
		uint64_t pri_offset;
		uint32_t pri_behavior, pri_user_wired_cnt, pri_user_tag, pri_pages_resident, pri_pages_shared_now_private, pri_pages_swapped_out, pri_pages_dirtied, pri_ref_cnt, pri_shadow_depth, pri_share_mode, pri_private_pages_resident, pri_shared_pages_resident, pri_obj_id, pri_depth;
		kaddr_t pri_addr;
		uint64_t pri_sz;
	} pri;
	mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
	CFDictionaryRef kexts_info, kext_info;
	kernrw_0_kbase_func_t kernrw_0_kbase;
	kaddr_t kext_addr, kext_addr_slid;
	task_dyld_info_data_t dyld_info;
	krw_0_kbase_func_t krw_0_kbase;
	char kext_name[KMOD_MAX_NAME];
	struct mach_header_64 mh64;
	CFStringRef kext_name_cf;
	CFNumberRef kext_addr_cf;
	CFArrayRef kext_names;

	if(kbase == 0) {
		if((((kernrw_0 == NULL || (kernrw_0_kbase = (kernrw_0_kbase_func_t)dlsym(kernrw_0, "kernRW_getKernelBase")) == NULL || kernrw_0_kbase(&kbase) != KERN_SUCCESS)) && (krw_0 == NULL || (krw_0_kbase = (krw_0_kbase_func_t)dlsym(krw_0, "kbase")) == NULL || krw_0_kbase(&kbase) != 0)) || tfp0 == TASK_NULL || task_info(tfp0, TASK_DYLD_INFO, (task_info_t)&dyld_info, &cnt) != KERN_SUCCESS || (kbase = vm_kernel_link_addr + dyld_info.all_image_info_size) == 0) {
			for(pri.pri_addr = 0; proc_pidinfo(0, PROC_PIDREGIONINFO, pri.pri_addr, &pri, sizeof(pri)) == sizeof(pri); pri.pri_addr += pri.pri_sz) {
				if(pri.pri_prot == VM_PROT_READ && pri.pri_user_tag == VM_KERN_MEMORY_OSKEXT) {
					if(kread_buf(pri.pri_addr + LOADED_KEXT_SUMMARY_HDR_NAME_OFF, kext_name, sizeof(kext_name)) == KERN_SUCCESS) {
						printf("kext_name: %s\n", kext_name);
						if(kread_addr(pri.pri_addr + LOADED_KEXT_SUMMARY_HDR_ADDR_OFF, &kext_addr_slid) == KERN_SUCCESS) {
							printf("kext_addr_slid: " KADDR_FMT "\n", kext_addr_slid);
							if((kext_name_cf = CFStringCreateWithCStringNoCopy(kCFAllocatorDefault, kext_name, kCFStringEncodingUTF8, kCFAllocatorNull)) != NULL) {
								if((kext_names = CFArrayCreate(kCFAllocatorDefault, (const void **)&kext_name_cf, 1, &kCFTypeArrayCallBacks)) != NULL) {
									if((kexts_info = OSKextCopyLoadedKextInfo(kext_names, NULL)) != NULL) {
										if(CFGetTypeID(kexts_info) == CFDictionaryGetTypeID() && CFDictionaryGetCount(kexts_info) == 1 && (kext_info = CFDictionaryGetValue(kexts_info, kext_name_cf)) != NULL && CFGetTypeID(kext_info) == CFDictionaryGetTypeID() && (kext_addr_cf = CFDictionaryGetValue(kext_info, CFSTR(kOSBundleLoadAddressKey))) != NULL && CFGetTypeID(kext_addr_cf) == CFNumberGetTypeID() && CFNumberGetValue(kext_addr_cf, kCFNumberSInt64Type, &kext_addr) && kext_addr_slid > kext_addr) {
											kbase = vm_kernel_link_addr + (kext_addr_slid - kext_addr);
										}
										CFRelease(kexts_info);
									}
									CFRelease(kext_names);
								}
								CFRelease(kext_name_cf);
							}
						}
					}
					break;
				}
			}
		}
	}
	if(kread_buf(kbase, &mh64, sizeof(mh64)) == KERN_SUCCESS && mh64.magic == MH_MAGIC_64 && mh64.cputype == CPU_TYPE_ARM64 && mh64.filetype ==
#if TARGET_OS_OSX
	   MH_FILESET
#else
	   MH_EXECUTE
#endif
	   ) {
		printf("kbase: " KADDR_FMT "\n", kbase);
		return KERN_SUCCESS;
	}
	return KERN_FAILURE;
}

static kern_return_t
pfinder_init_offsets(void) {
	kern_return_t ret = KERN_FAILURE;
	struct utsname uts;
	CFStringRef cf_str;
	pfinder_t pfinder;
	char *p, *e;

	if(uname(&uts) == 0 && (p = strstr(uts.version, "root:xnu-")) != NULL && (e = strchr(p += strlen("root:xnu-"), '~')) != NULL) {
		*e = '\0';
		if((cf_str = CFStringCreateWithCStringNoCopy(kCFAllocatorDefault, p, kCFStringEncodingASCII, kCFAllocatorNull)) != NULL) {
			task_map_off = 0x20;
			proc_task_off = 0x18;
			proc_p_pid_off = 0x10;
			vm_map_pmap_off = 0x48;
			pmap_sw_asid_off = 0x28;
			vm_map_flags_off = 0x110;
#if TARGET_OS_OSX
			vm_kernel_link_addr = 0xFFFFFE0007004000ULL;
#else
			vm_kernel_link_addr = 0xFFFFFFF007004000ULL;
#endif
			if(CFStringCompare(cf_str, CFSTR("4903.200.199.12.3"), kCFCompareNumerically) != kCFCompareLessThan) {
				proc_task_off = 0x10;
				proc_p_pid_off = 0x60;
				pmap_sw_asid_off = 0xDC;
				vm_map_flags_off = 0x10C;
				if(CFStringCompare(cf_str, CFSTR("6041.0.0.110.11"), kCFCompareNumerically) != kCFCompareLessThan) {
					task_map_off = 0x28;
					pmap_sw_asid_off = 0xEE;
					if(CFStringCompare(cf_str, CFSTR("6110.0.0.120.8"), kCFCompareNumerically) != kCFCompareLessThan) {
						proc_p_pid_off = 0x68;
						if(CFStringCompare(cf_str, CFSTR("6153.40.121.0.1"), kCFCompareNumerically) != kCFCompareLessThan) {
							pmap_sw_asid_off = 0xE6;
							if(CFStringCompare(cf_str, CFSTR("7090.0.0.110.4"), kCFCompareNumerically) != kCFCompareLessThan) {
								pmap_sw_asid_off = 0xDE;
								if(CFStringCompare(cf_str, CFSTR("7195.100.326.0.1"), kCFCompareNumerically) != kCFCompareLessThan) {
									task_map_off = 0x20;
									if(CFStringCompare(cf_str, CFSTR("7938.0.0.111.2"), kCFCompareNumerically) != kCFCompareLessThan) {
										task_map_off = 0x28;
										pmap_sw_asid_off = 0x96;
										if(CFStringCompare(cf_str, CFSTR("8011.0.0.121.4"), kCFCompareNumerically) != kCFCompareLessThan) {
											vm_map_flags_off = 0x11C;
											if(CFStringCompare(cf_str, CFSTR("8020.100.406.0.1"), kCFCompareNumerically) != kCFCompareLessThan) {
												vm_map_pmap_off = 0x40;
												vm_map_flags_off = 0x94;
												if(CFStringCompare(cf_str, CFSTR("8792.0.50.111.3"), kCFCompareNumerically) != kCFCompareLessThan) {
													proc_p_pid_off = 0x60;
													pmap_sw_asid_off = 0x8E;
													vm_map_flags_off = 0xB4;
													has_vm_obj_packed_ptr = has_proc_struct_sz = true;
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
			CFRelease(cf_str);
			if(init_kbase() == KERN_SUCCESS && pfinder_init(&pfinder) == KERN_SUCCESS) {
				if((kernproc = pfinder_kernproc(pfinder)) != 0) {
					printf("kernproc: " KADDR_FMT "\n", kernproc);
					if((pv_head_table_ptr = pfinder_pv_head_table_ptr(pfinder)) != 0) {
						printf("pv_head_table_ptr: " KADDR_FMT "\n", pv_head_table_ptr);
						if((const_boot_args = pfinder_const_boot_args(pfinder)) != 0) {
							printf("const_boot_args: " KADDR_FMT "\n", const_boot_args);
							if((lowglo_ptr = pfinder_lowglo_ptr(pfinder)) != 0) {
								printf("lowglo_ptr: " KADDR_FMT "\n", lowglo_ptr);
								if(!has_proc_struct_sz) {
									ret = KERN_SUCCESS;
								} else if((proc_struct_sz_ptr = pfinder_proc_struct_sz_ptr(pfinder)) != 0) {
									printf("proc_struct_sz_ptr: " KADDR_FMT "\n", proc_struct_sz_ptr);
									ret = KERN_SUCCESS;
								}
							}
						}
					}
				}
				pfinder_term(&pfinder);
			}
		}
	}
	return ret;
}

static kern_return_t
find_task(pid_t pid, kaddr_t *task) {
	pid_t cur_pid;
	kaddr_t proc;

	if(kread_addr(kernproc + PROC_P_LIST_LH_FIRST_OFF, &proc) == KERN_SUCCESS) {
		while(proc != 0 && kread_buf(proc + proc_p_pid_off, &cur_pid, sizeof(cur_pid)) == KERN_SUCCESS) {
			if(cur_pid == pid) {
				if(has_proc_struct_sz) {
					*task = proc + proc_struct_sz;
					return KERN_SUCCESS;
				}
				return kread_addr(proc + proc_task_off, task);
			}
			if(pid == 0 || kread_addr(proc + PROC_P_LIST_LE_PREV_OFF, &proc) != KERN_SUCCESS) {
				break;
			}
		}
	}
	return KERN_FAILURE;
}

static kern_return_t
vm_map_lookup_entry(kaddr_t vm_map, kaddr_t virt, vm_map_entry_t *vm_entry) {
	kaddr_t rb_entry;

	if(kread_addr(vm_map + VM_MAP_HDR_RBH_ROOT_OFF, &rb_entry) == KERN_SUCCESS) {
		while(rb_entry != 0 && rb_entry != sizeof(vm_entry->links)) {
			printf("rb_entry: " KADDR_FMT "\n", rb_entry);
			if(kread_buf(rb_entry - sizeof(vm_entry->links), vm_entry, sizeof(*vm_entry)) != KERN_SUCCESS) {
				break;
			}
			printf("start: " KADDR_FMT ", end: " KADDR_FMT ", vme_object: " KADDR_FMT ", vme_offset: 0x%" PRIX64 "\n", vm_entry->links.start, vm_entry->links.end, vm_entry->vme_object, vm_entry->vme_offset);
			if(virt >= vm_entry->links.start) {
				if(virt < vm_entry->links.end) {
					return KERN_SUCCESS;
				}
				rb_entry = vm_entry->rbe_right;
			} else {
				rb_entry = vm_entry->rbe_left;
			}
		}
	}
	return KERN_FAILURE;
}

static kaddr_t
vm_obj_unpack_ptr(kaddr_t p) {
	return has_vm_obj_packed_ptr ? lowglo.pmap_mem_packed_base_addr + (uint64_t)((int64_t)(p >> (32U - lowglo.pmap_mem_packed_shift))) : p;
}

static kaddr_t
vm_page_unpack_ptr(kaddr_t p) {
	if(p != 0) {
		if((p & lowglo.pmap_mem_from_array_mask) != 0) {
			return lowglo.pmap_mem_start_addr + lowglo.pmap_mem_page_sz * (p & ~lowglo.pmap_mem_from_array_mask);
		}
		return lowglo.pmap_mem_packed_base_addr + (p << lowglo.pmap_mem_packed_shift);
	}
	return 0;
}

static kaddr_t
vm_page_get_phys_addr(kaddr_t vm_page) {
	ppnum_t phys_page;

	if(vm_page >= lowglo.pmap_mem_start_addr && vm_page < lowglo.pmap_mem_end_addr) {
		phys_page = (ppnum_t)((vm_page - lowglo.pmap_mem_start_addr) / lowglo.pmap_mem_page_sz + lowglo.pmap_mem_first_ppnum);
	} else if(kread_buf(vm_page + lowglo.pmap_mem_page_sz, &phys_page, sizeof(phys_page)) != KERN_SUCCESS) {
		phys_page = 0;
	}
	return (kaddr_t)phys_page << vm_kernel_page_shift;
}

void
golb_term(void) {
	if(tfp0 != TASK_NULL) {
		mach_port_deallocate(mach_task_self(), tfp0);
	} else if(kernrw_0 != NULL) {
		dlclose(kernrw_0);
	} else if(krw_0 != NULL) {
		dlclose(krw_0);
	} else if(kmem_fd != -1) {
		close(kmem_fd);
	}
	setpriority(PRIO_PROCESS, 0, 0);
}

kern_return_t
golb_init(kaddr_t _kbase, kread_func_t _kread_buf, kwrite_func_t _kwrite_buf) {
	kernrw_0_req_kernrw_func_t kernrw_0_req;
	cpu_subtype_t subtype;
	kaddr_t our_task;
	size_t sz;

	sz = sizeof(subtype);
	if(sysctlbyname("hw.cpusubtype", &subtype, &sz, NULL, 0) == 0) {
		if(subtype == CPU_SUBTYPE_ARM64E) {
#if TARGET_OS_OSX
			t1sz_boot = 17;
#else
			t1sz_boot = 25;
#endif
		}
		kbase = _kbase;
		if(_kread_buf != NULL && _kwrite_buf != NULL) {
			kread_buf = _kread_buf;
			kwrite_buf = _kwrite_buf;
		} else if(init_tfp0() == KERN_SUCCESS) {
			printf("tfp0: 0x%" PRIX32 "\n", tfp0);
			kread_buf = kread_buf_tfp0;
			kwrite_buf = kwrite_buf_tfp0;
		} else if((kernrw_0 = dlopen("/usr/lib/libkernrw.0.dylib", RTLD_LAZY)) != NULL && (kernrw_0_req = (kernrw_0_req_kernrw_func_t)dlsym(kernrw_0, "requestKernRw")) != NULL && kernrw_0_req() == 0) {
			kread_buf = (kread_func_t)dlsym(kernrw_0, "kernRW_readbuf");
			kwrite_buf = (kwrite_func_t)dlsym(kernrw_0, "kernRW_writebuf");
		} else if((krw_0 = dlopen("/usr/lib/libkrw.0.dylib", RTLD_LAZY)) != NULL && (krw_0_kread = (krw_0_kread_func_t)dlsym(krw_0, "kread")) != NULL && (krw_0_kwrite = (krw_0_kwrite_func_t)dlsym(krw_0, "kwrite")) != NULL) {
			kread_buf = kread_buf_krw_0;
			kwrite_buf = kwrite_buf_krw_0;
		} else if((kmem_fd = open("/dev/kmem", O_RDWR | O_CLOEXEC)) != -1) {
			kread_buf = kread_buf_kmem;
			kwrite_buf = kwrite_buf_kmem;
		}
		if(kread_buf != NULL && kwrite_buf != NULL) {
			setpriority(PRIO_PROCESS, 0, PRIO_MIN);
			if(init_arm_pgshift() == KERN_SUCCESS) {
				printf("arm_pgshift: %u\n", arm_pgshift);
				if(pfinder_init_offsets() == KERN_SUCCESS && kread_buf(lowglo_ptr, &lowglo, sizeof(lowglo)) == KERN_SUCCESS && kread_addr(pv_head_table_ptr, &pv_head_table) == KERN_SUCCESS && (!has_proc_struct_sz || kread_buf(proc_struct_sz_ptr, &proc_struct_sz, sizeof(proc_struct_sz)) == KERN_SUCCESS)) {
					printf("pv_head_table: " KADDR_FMT "\n", pv_head_table);
					if(kread_buf(const_boot_args, &boot_args, sizeof(boot_args)) == KERN_SUCCESS) {
						printf("virt_base: " KADDR_FMT ", phys_base: " KADDR_FMT ", mem_sz: 0x%" PRIX64 "\n", boot_args.virt_base, boot_args.phys_base, boot_args.mem_sz);
						if(find_task(getpid(), &our_task) == KERN_SUCCESS) {
							kxpacd(&our_task);
							printf("our_task: " KADDR_FMT "\n", our_task);
							if(kread_addr(our_task + task_map_off, &our_map) == KERN_SUCCESS) {
								kxpacd(&our_map);
								printf("our_map: " KADDR_FMT "\n", our_map);
								if(kread_addr(our_map + vm_map_pmap_off, &our_pmap) == KERN_SUCCESS) {
									kxpacd(&our_pmap);
									printf("our_pmap: " KADDR_FMT "\n", our_pmap);
									return KERN_SUCCESS;
								}
							}
						}
					}
				}
			}
			setpriority(PRIO_PROCESS, 0, 0);
		}
		if(tfp0 != TASK_NULL) {
			mach_port_deallocate(mach_task_self(), tfp0);
		} else if(kernrw_0 != NULL) {
			dlclose(kernrw_0);
		} else if(krw_0 != NULL) {
			dlclose(krw_0);
		} else if(kmem_fd != -1) {
			close(kmem_fd);
		}
	}
	return KERN_FAILURE;
}

kern_return_t
golb_flush_core_tlb_asid(void) {
	uint8_t orig_sw_asid, fake_sw_asid = UINT8_MAX;

	if(kread_buf(our_pmap + pmap_sw_asid_off, &orig_sw_asid, sizeof(orig_sw_asid)) == KERN_SUCCESS) {
		printf("orig_sw_asid: 0x%" PRIX8 "\n", orig_sw_asid);
		if(orig_sw_asid != fake_sw_asid && kwrite_buf(our_pmap + pmap_sw_asid_off, &fake_sw_asid, sizeof(fake_sw_asid)) == KERN_SUCCESS) {
			return kwrite_buf(our_pmap + pmap_sw_asid_off, &orig_sw_asid, sizeof(orig_sw_asid));
		}
	}
	return KERN_FAILURE;
}

kaddr_t
golb_find_phys(kaddr_t virt) {
	kaddr_t vphys, vm_page, virt_off = virt & vm_page_mask;
	vm_map_entry_t vm_entry;
	vm_page_t m;

	virt -= virt_off;
	if(vm_map_lookup_entry(our_map, virt, &vm_entry) == KERN_SUCCESS && (vm_entry.vme_object = vm_obj_unpack_ptr(vm_entry.vme_object)) != 0 && trunc_page_kernel(vm_entry.vme_offset) == 0 && kread_buf(vm_entry.vme_object, &m.vmp_listq.next, sizeof(m.vmp_listq.next)) == KERN_SUCCESS) {
		while((vm_page = vm_page_unpack_ptr(m.vmp_listq.next)) != 0) {
			printf("vm_page: " KADDR_FMT "\n", vm_page);
			if(vm_page == vm_entry.vme_object || kread_buf(vm_page, &m, sizeof(m)) != KERN_SUCCESS) {
				break;
			}
			printf("vmp_offset: 0x%" PRIX64 ", vmp_object: 0x%" PRIX32 "\n", m.vmp_offset, m.vmp_object);
			if(m.vmp_offset == virt - vm_entry.links.start && vm_page_unpack_ptr(m.vmp_object) == vm_entry.vme_object && (vphys = vm_page_get_phys_addr(vm_page)) != 0) {
				return vphys + virt_off;
			}
		}
	}
	return 0;
}

void
golb_unmap(golb_ctx_t ctx) {
	size_t i;

	for(i = 0; i < ctx.page_cnt; ++i) {
		kwrite_addr(ctx.pages[i].ptep, ctx.pages[i].pte);
	}
	golb_flush_core_tlb_asid();
	free(ctx.pages);
	mach_vm_deallocate(mach_task_self(), trunc_page(ctx.virt), ctx.page_cnt << arm_pgshift);
}

kern_return_t
golb_map(golb_ctx_t *ctx, kaddr_t phys, mach_vm_size_t sz, vm_prot_t prot) {
	kaddr_t phys_off = phys & vm_page_mask, vm_page, vphys = 0, pv_h, ptep = 0, pte;
	mach_vm_offset_t map_off;
	vm_map_entry_t vm_entry;
	uint32_t flags;
	vm_page_t m;

	phys -= phys_off;
	if((sz = round_page(sz + phys_off)) != 0 && mach_vm_allocate(mach_task_self(), &ctx->virt, sz, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
		printf("virt: " KADDR_FMT "\n", ctx->virt);
		if(kread_buf(our_map + vm_map_flags_off, &flags, sizeof(flags)) == KERN_SUCCESS) {
			printf("flags: 0x%" PRIX32 "\n", flags);
			flags |= VM_MAP_FLAGS_NO_ZERO_FILL;
			if(kwrite_buf(our_map + vm_map_flags_off, &flags, sizeof(flags)) == KERN_SUCCESS) {
				for(map_off = 0; map_off < sz; map_off += vm_kernel_page_size) {
					*(volatile kaddr_t *)(ctx->virt + map_off) = FAULT_MAGIC;
				}
				flags &= ~VM_MAP_FLAGS_NO_ZERO_FILL;
				if(kwrite_buf(our_map + vm_map_flags_off, &flags, sizeof(flags)) == KERN_SUCCESS && vm_map_lookup_entry(our_map, ctx->virt, &vm_entry) == KERN_SUCCESS && (vm_entry.vme_object = vm_obj_unpack_ptr(vm_entry.vme_object)) != 0 && trunc_page_kernel(vm_entry.vme_offset) == 0 && vm_entry.links.end - ctx->virt >= sz && kread_buf(vm_entry.vme_object, &m.vmp_listq.next, sizeof(m.vmp_listq.next)) == KERN_SUCCESS && (ctx->pages = calloc(sz >> arm_pgshift, sizeof(ctx->pages[0]))) != NULL) {
					ctx->page_cnt = 0;
					for(map_off = 0; map_off < sz; map_off += ARM_PGBYTES) {
						if((map_off & vm_kernel_page_mask) == 0) {
							if((vm_page = vm_page_unpack_ptr(m.vmp_listq.next)) == 0) {
								break;
							}
							printf("vm_page: " KADDR_FMT "\n", vm_page);
							if(vm_page == vm_entry.vme_object || kread_buf(vm_page, &m, sizeof(m)) != KERN_SUCCESS) {
								break;
							}
							printf("vmp_offset: 0x%" PRIX64 ", vmp_object: 0x%" PRIX32 "\n", m.vmp_offset, m.vmp_object);
							if(m.vmp_offset != map_off + (ctx->virt - vm_entry.links.start) || vm_page_unpack_ptr(m.vmp_object) != vm_entry.vme_object || (vphys = vm_page_get_phys_addr(vm_page)) == 0) {
								break;
							}
							printf("vphys: " KADDR_FMT "\n", vphys);
							if(vphys < boot_args.phys_base || vphys >= trunc_page_kernel(boot_args.phys_base + boot_args.mem_sz) || kread_addr(pv_head_table + ((vphys - boot_args.phys_base) >> vm_kernel_page_shift) * sizeof(pv_h), &pv_h) != KERN_SUCCESS) {
								break;
							}
							printf("pv_h: " KADDR_FMT "\n", pv_h);
							if((pv_h & PVH_TYPE_MASK) != PVH_TYPE_PTEP) {
								break;
							}
							ptep = (pv_h & PVH_LIST_MASK) | PVH_HIGH_FLAGS;
						} else {
							vphys += ARM_PGBYTES;
							ptep += sizeof(pte);
						}
						printf("ptep: " KADDR_FMT "\n", ptep);
						if(kread_addr(ptep, &pte) != KERN_SUCCESS) {
							break;
						}
						printf("pte: " KADDR_FMT "\n", pte);
						if((pte & (ARM_PTE_MASK | ARM_PTE_TYPE)) != (vphys | ARM_PTE_TYPE_VALID)) {
							break;
						}
						ctx->pages[ctx->page_cnt].ptep = ptep;
						ctx->pages[ctx->page_cnt++].pte = pte;
						pte = ((phys + map_off) & ARM_PTE_MASK) | ARM_PTE_TYPE_VALID | ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DISABLE) | ARM_PTE_AF | ARM_PTE_AP((prot & VM_PROT_WRITE) != 0 ? AP_RWRW : AP_RORO) | ARM_PTE_PNX | ARM_PTE_NG;
						if((prot & VM_PROT_EXECUTE) == 0) {
							pte |= ARM_PTE_NX;
						}
						if(kwrite_addr(ptep, pte) != KERN_SUCCESS) {
							break;
						}
					}
					if(map_off != sz || golb_flush_core_tlb_asid() != KERN_SUCCESS) {
						while(ctx->page_cnt-- != 0) {
							kwrite_addr(ctx->pages[ctx->page_cnt].ptep, ctx->pages[ctx->page_cnt].pte);
						}
						golb_flush_core_tlb_asid();
					} else {
						ctx->virt += phys_off;
						return KERN_SUCCESS;
					}
					free(ctx->pages);
				}
			}
		}
		mach_vm_deallocate(mach_task_self(), ctx->virt, sz);
	}
	return KERN_FAILURE;
}
