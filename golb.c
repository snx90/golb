/* Copyright 2020 0x7ff
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
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#define LZSS_F (18)
#define LZSS_N (4096)
#define LZSS_THRESHOLD (2)
#define VM_MAP_PMAP_OFF (0x48)
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
#define kIODeviceTreePlane "IODeviceTree"
#define PVH_FLAG_PPL_HASHED (1ULL << 58U)
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
#define LDR_W_UNSIGNED_IMM(a) (extract32(a, 10, 12) << 2U)
#define LDR_X_UNSIGNED_IMM(a) (extract32(a, 10, 12) << 3U)
#define IS_LDR_W_UNSIGNED_IMM(a) (((a) & 0xFFC00000U) == 0xB9400000U)
#define IS_LDR_X_UNSIGNED_IMM(a) (((a) & 0xFFC00000U) == 0xF9400000U)
#define ADR_IMM(a) ((sextract64(a, 5, 19) << 2U) | extract32(a, 29, 2))
#define ARM_PTE_MASK (((1ULL << ARM64_VMADDR_BITS) - 1U) & ~ARM_PGMASK)

#ifndef SECT_CSTRING
#	define SECT_CSTRING "__cstring"
#endif

#ifndef SEG_TEXT_EXEC
#	define SEG_TEXT_EXEC "__TEXT_EXEC"
#endif

#ifndef MIN
#	define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef struct {
	struct section_64 s64;
	const char *data;
} sec_64_t;

typedef struct {
	uint16_t rev, ver;
	uint32_t pad;
	kaddr_t virt_base, phys_base;
	uint64_t mem_sz;
} boot_args_t;

typedef struct {
	sec_64_t sec_text, sec_data, sec_cstring;
	struct symtab_command cmd_symtab;
	const char *kernel;
	kaddr_t base, pc;
	size_t kernel_sz;
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
static unsigned arm_pgshift;
static boot_args_t boot_args;
static kread_func_t kread_buf;
static task_t tfp0 = TASK_NULL;
static kwrite_func_t kwrite_buf;
static size_t task_map_off, proc_task_off, proc_p_pid_off, pmap_sw_asid_off, vm_map_flags_off;
static kaddr_t kslide, pvh_high_flags, kernproc, pv_head_table_ptr, const_boot_args, lowglo_ptr, pv_head_table, our_map, our_pmap;

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
#if defined(__arm64e__) || TARGET_OS_OSX
	__asm__ volatile("xpacd %0" : "+r"(*addr));
#else
	(void)addr;
#endif
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
init_tfp0(void) {
	kern_return_t ret = task_for_pid(mach_task_self(), 0, &tfp0);
	mach_port_t host;

	if(ret != KERN_SUCCESS) {
		host = mach_host_self();
		if(MACH_PORT_VALID(host)) {
			printf("host: 0x%" PRIX32 "\n", host);
			ret = host_get_special_port(host, HOST_LOCAL_NODE, 4, &tfp0);
			mach_port_deallocate(mach_task_self(), host);
		}
	}
	if(ret == KERN_SUCCESS && MACH_PORT_VALID(tfp0)) {
		return ret;
	}
	return KERN_FAILURE;
}

static kern_return_t
kread_buf_tfp0(kaddr_t addr, void *buf, mach_vm_size_t sz) {
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
kwrite_buf_tfp0(kaddr_t addr, const void *buf, mach_msg_type_number_t sz) {
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
kread_buf_kmem(kaddr_t addr, void *buf, mach_vm_size_t sz) {
	ssize_t n = pread(kmem_fd, buf, sz, (off_t)addr);

	if(n > 0 && (mach_vm_size_t)n == sz) {
		return KERN_SUCCESS;
	}
	return KERN_FAILURE;
}

static kern_return_t
kwrite_buf_kmem(kaddr_t addr, const void *buf, mach_msg_type_number_t sz) {
	ssize_t n = pwrite(kmem_fd, buf, sz, (off_t)addr);

	if(n > 0 && (mach_msg_type_number_t)n == sz) {
		return KERN_SUCCESS;
	}
	return KERN_FAILURE;
}

static kern_return_t
kwrite_addr(kaddr_t addr, kaddr_t val) {
	return kwrite_buf(addr, &val, sizeof(val));
}

static kern_return_t
find_section(const char *p, struct segment_command_64 sg64, const char *sect_name, struct section_64 *sp) {
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
	pfinder->base = 0;
	pfinder->data = NULL;
	pfinder->kernel = NULL;
	pfinder->kernel_sz = 0;
	sec_reset(&pfinder->sec_text);
	sec_reset(&pfinder->sec_data);
	sec_reset(&pfinder->sec_cstring);
	memset(&pfinder->cmd_symtab, '\0', sizeof(pfinder->cmd_symtab));
}

static void
pfinder_term(pfinder_t *pfinder) {
	free(pfinder->data);
	pfinder_reset(pfinder);
}

static kern_return_t
pfinder_init_macho(pfinder_t *pfinder, size_t off) {
	const char *p = pfinder->kernel + off, *e;
	struct {
		uint32_t flavor, cnt;
		kaddr_t x[29], fp, lr, sp, pc;
		uint32_t cpsr, pad;
	} state;
#if TARGET_OS_OSX
	struct fileset_entry_command fec;
#endif
	struct symtab_command cmd_symtab;
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
				if(sg64.fileoff == 0 && sg64.filesize != 0) {
					pfinder->base = sg64.vmaddr;
					printf("base: " KADDR_FMT "\n", sg64.vmaddr);
				}
				if(mh64.filetype == MH_EXECUTE) {
					if(strncmp(sg64.segname, SEG_TEXT_EXEC, sizeof(sg64.segname)) == 0) {
						if(find_section(p + sizeof(sg64), sg64, SECT_TEXT, &s64) != KERN_SUCCESS) {
							break;
						}
						pfinder->sec_text.s64 = s64;
						pfinder->sec_text.data = pfinder->kernel + s64.offset;
						printf("sec_text_addr: " KADDR_FMT ", sec_text_off: 0x%" PRIX32 ", sec_text_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					} else if(strncmp(sg64.segname, SEG_DATA, sizeof(sg64.segname)) == 0) {
						if(find_section(p + sizeof(sg64), sg64, SECT_DATA, &s64) != KERN_SUCCESS) {
							break;
						}
						pfinder->sec_data.s64 = s64;
						pfinder->sec_data.data = pfinder->kernel + s64.offset;
						printf("sec_data_addr: " KADDR_FMT ", sec_data_off: 0x%" PRIX32 ", sec_data_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					} else if(strncmp(sg64.segname, SEG_TEXT, sizeof(sg64.segname)) == 0) {
						if(find_section(p + sizeof(sg64), sg64, SECT_CSTRING, &s64) != KERN_SUCCESS || pfinder->kernel[s64.offset + s64.size - 1] != '\0') {
							break;
						}
						pfinder->sec_cstring.s64 = s64;
						pfinder->sec_cstring.data = pfinder->kernel + s64.offset;
						printf("sec_cstring_addr: " KADDR_FMT ", sec_cstring_off: 0x%" PRIX32 ", sec_cstring_sz: 0x%" PRIX64 "\n", s64.addr, s64.offset, s64.size);
					}
				}
			} else if(lc.cmd == LC_SYMTAB) {
				if(lc.cmdsize != sizeof(cmd_symtab)) {
					break;
				}
				memcpy(&cmd_symtab, p, sizeof(cmd_symtab));
				printf("cmd_symtab_symoff: 0x%" PRIX32 ", cmd_symtab_nsyms: 0x%" PRIX32 ", cmd_symtab_stroff: 0x%" PRIX32 "\n", cmd_symtab.symoff, cmd_symtab.nsyms, cmd_symtab.stroff);
				if(cmd_symtab.nsyms != 0 && (cmd_symtab.symoff > pfinder->kernel_sz || cmd_symtab.nsyms > (pfinder->kernel_sz - cmd_symtab.symoff) / sizeof(struct nlist_64) || cmd_symtab.stroff > pfinder->kernel_sz || cmd_symtab.strsize > pfinder->kernel_sz - cmd_symtab.stroff || cmd_symtab.strsize == 0 || pfinder->kernel[cmd_symtab.stroff + cmd_symtab.strsize - 1] != '\0')) {
					break;
				}
				pfinder->cmd_symtab = cmd_symtab;
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
			if(pfinder->base != 0 && pfinder->sec_text.s64.size != 0 && pfinder->sec_data.s64.size != 0 && pfinder->sec_cstring.s64.size != 0 && pfinder->cmd_symtab.cmdsize != 0 && pfinder->pc != 0) {
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
				if(x[rd] < pfinder.base) {
					break;
				}
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
pfinder_sym(pfinder_t pfinder, const char *sym) {
	const char *p, *strtab = pfinder.kernel + pfinder.cmd_symtab.stroff;
	struct nlist_64 nl64;

	for(p = pfinder.kernel + pfinder.cmd_symtab.symoff; pfinder.cmd_symtab.nsyms-- != 0; p += sizeof(nl64)) {
		memcpy(&nl64, p, sizeof(nl64));
		if(nl64.n_un.n_strx != 0 && nl64.n_un.n_strx < pfinder.cmd_symtab.strsize && (nl64.n_type & (N_STAB | N_TYPE)) == N_SECT && nl64.n_value >= pfinder.base && strcmp(strtab + nl64.n_un.n_strx, sym) == 0) {
			return nl64.n_value + kslide;
		}
	}
	return 0;
}

static kaddr_t
pfinder_kernproc(pfinder_t pfinder) {
	kaddr_t ref = pfinder_sym(pfinder, "_kernproc");
	uint32_t insns[2];

	if(ref != 0) {
		return ref;
	}
	for(ref = pfinder_xref_str(pfinder, "\"Should never have an EVFILT_READ except for reg or fifo.\"", 0); sec_read_buf(pfinder.sec_text, ref, insns, sizeof(insns)) == KERN_SUCCESS; ref -= sizeof(*insns)) {
		if(IS_ADRP(insns[0]) && IS_LDR_X_UNSIGNED_IMM(insns[1]) && RD(insns[1]) == 3) {
			return pfinder_xref_rd(pfinder, RD(insns[1]), ref, 0);
		}
	}
	return 0;
}

static kaddr_t
pfinder_pv_head_table_ptr(pfinder_t pfinder) {
	kaddr_t ref = pfinder_sym(pfinder, "_pv_head_table");
	uint32_t insns[3];

	if(ref != 0) {
		return ref;
	}
	if((ref = pfinder_xref_str(pfinder, "\"pmap_init_pte_page(): invalid PVH type for pte_p %p\"", 0)) != 0) {
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
	kaddr_t ref = pfinder_sym(pfinder, "_const_boot_args");

	return ref != 0 ? ref : pfinder_xref_rd(pfinder, 20, ADRP_ADDR(pfinder.pc), 0);
}

static kaddr_t
pfinder_lowglo_ptr(pfinder_t pfinder) {
	kaddr_t ref = pfinder_sym(pfinder, "_lowGlo");

	if(ref != 0) {
		return ref;
	}
	for(ref = pfinder.sec_data.s64.addr; sec_read_buf(pfinder.sec_data, ref, &lowglo, sizeof(lowglo)) == KERN_SUCCESS; ref += PAGE_MAX_SIZE) {
		if(memcmp(&lowglo.ver_code, LOWGLO_VER_CODE, sizeof(lowglo.ver_code)) == 0 && lowglo.layout_magic == LOWGLO_LAYOUT_MAGIC && lowglo.pmap_mem_page_sz == sizeof(vm_page_t)) {
			return ref;
		}
	}
	return 0;
}

static kaddr_t
pfinder_init_kbase(pfinder_t *pfinder) {
	struct {
		uint32_t pri_protection, pri_max_protection, pri_inheritance, pri_flags;
		uint64_t pri_offset;
		uint32_t pri_behavior, pri_user_wired_count, pri_user_tag, pri_pages_resident, pri_pages_shared_now_private, pri_pages_swapped_out, pri_pages_dirtied, pri_ref_count, pri_shadow_depth, pri_share_mode, pri_private_pages_resident, pri_shared_pages_resident, pri_obj_id, pri_depth;
		uint64_t pri_address, pri_size;
	} pri;
	mach_msg_type_number_t cnt = TASK_DYLD_INFO_COUNT;
	kaddr_t addr, kext_addr, kext_addr_slid;
	CFDictionaryRef kexts_info, kext_info;
	task_dyld_info_data_t dyld_info;
	char kext_name[KMOD_MAX_NAME];
	struct mach_header_64 mh64;
	CFStringRef kext_name_cf;
	CFNumberRef kext_addr_cf;
	CFArrayRef kext_names;

	if(kslide == 0) {
		if(tfp0 != TASK_NULL && task_info(tfp0, TASK_DYLD_INFO, (task_info_t)&dyld_info, &cnt) == KERN_SUCCESS) {
			kslide = dyld_info.all_image_info_size;
		}
		if(kslide == 0) {
			for(addr = 0; proc_pidinfo(0, PROC_PIDREGIONINFO, addr, &pri, sizeof(pri)) == sizeof(pri); addr += pri.pri_size) {
				addr = pri.pri_address;
				if(pri.pri_protection == VM_PROT_READ && pri.pri_user_tag == VM_KERN_MEMORY_OSKEXT) {
					if(kread_buf(addr + LOADED_KEXT_SUMMARY_HDR_NAME_OFF, kext_name, sizeof(kext_name)) == KERN_SUCCESS) {
						printf("kext_name: %s\n", kext_name);
						if(kread_addr(addr + LOADED_KEXT_SUMMARY_HDR_ADDR_OFF, &kext_addr_slid) == KERN_SUCCESS) {
							printf("kext_addr_slid: " KADDR_FMT "\n", kext_addr_slid);
							if((kext_name_cf = CFStringCreateWithCStringNoCopy(kCFAllocatorDefault, kext_name, kCFStringEncodingUTF8, kCFAllocatorNull)) != NULL) {
								if((kext_names = CFArrayCreate(kCFAllocatorDefault, (const void **)&kext_name_cf, 1, &kCFTypeArrayCallBacks)) != NULL) {
									if((kexts_info = OSKextCopyLoadedKextInfo(kext_names, NULL)) != NULL) {
										if(CFGetTypeID(kexts_info) == CFDictionaryGetTypeID() && CFDictionaryGetCount(kexts_info) == 1 && (kext_info = CFDictionaryGetValue(kexts_info, kext_name_cf)) != NULL && CFGetTypeID(kext_info) == CFDictionaryGetTypeID() && (kext_addr_cf = CFDictionaryGetValue(kext_info, CFSTR(kOSBundleLoadAddressKey))) != NULL && CFGetTypeID(kext_addr_cf) == CFNumberGetTypeID() && CFNumberGetValue(kext_addr_cf, kCFNumberSInt64Type, &kext_addr) && kext_addr_slid > kext_addr) {
											kslide = kext_addr_slid - kext_addr;
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
	if(pfinder->base + kslide > pfinder->base && kread_buf(pfinder->base + kslide, &mh64, sizeof(mh64)) == KERN_SUCCESS && mh64.magic == MH_MAGIC_64 && mh64.cputype == CPU_TYPE_ARM64 && mh64.filetype ==
#if TARGET_OS_OSX
	   MH_FILESET
#else
	   MH_EXECUTE
#endif
	   ) {
		pfinder->pc += kslide;
		pfinder->sec_text.s64.addr += kslide;
		pfinder->sec_data.s64.addr += kslide;
		pfinder->sec_cstring.s64.addr += kslide;
		printf("kbase: " KADDR_FMT ", kslide: " KADDR_FMT "\n", pfinder->base + kslide, kslide);
		return KERN_SUCCESS;
	}
	return KERN_FAILURE;
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

	if(stat(BOOT_PATH, &stat_buf) != -1 && S_ISREG(stat_buf.st_mode)) {
		path = malloc(path_len);
	} else if(stat(PREBOOT_PATH, &stat_buf) != -1 && S_ISDIR(stat_buf.st_mode) && (chosen = IORegistryEntryFromPath(kIOMasterPortDefault, kIODeviceTreePlane ":/chosen")) != IO_OBJECT_NULL) {
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
	if(path != NULL) {
		memcpy(path + (path_len - sizeof(BOOT_PATH)), BOOT_PATH, sizeof(BOOT_PATH));
	}
	return path;
}

static kern_return_t
pfinder_init_offsets(void) {
	kern_return_t ret = KERN_FAILURE;
	char *p, *e, *boot_path;
	struct utsname uts;
	CFStringRef cf_str;
	pfinder_t pfinder;

	if(uname(&uts) == 0 && (p = strstr(uts.version, "root:xnu-")) != NULL && (e = strchr(p += strlen("root:xnu-"), '~')) != NULL) {
		*e = '\0';
		if((cf_str = CFStringCreateWithCStringNoCopy(kCFAllocatorDefault, p, kCFStringEncodingASCII, kCFAllocatorNull)) != NULL) {
			pvh_high_flags = 0;
			task_map_off = 0x20;
			proc_task_off = 0x18;
			proc_p_pid_off = 0x10;
			pmap_sw_asid_off = 0x28;
			vm_map_flags_off = 0x110;
			if(CFStringCompare(cf_str, CFSTR("4903.200.199.12.3"), kCFCompareNumerically) != kCFCompareLessThan) {
				proc_task_off = 0x10;
				proc_p_pid_off = 0x60;
				pmap_sw_asid_off = 0xDC;
				vm_map_flags_off = 0x10C;
				pvh_high_flags = PVH_FLAG_CPU | PVH_FLAG_LOCK | PVH_FLAG_EXEC | PVH_FLAG_LOCKDOWN;
				if(CFStringCompare(cf_str, CFSTR("6041.0.0.110.11"), kCFCompareNumerically) != kCFCompareLessThan) {
					task_map_off = 0x28;
					pmap_sw_asid_off = 0xEE;
					if(CFStringCompare(cf_str, CFSTR("6110.0.0.120.8"), kCFCompareNumerically) != kCFCompareLessThan) {
						proc_p_pid_off = 0x68;
						if(CFStringCompare(cf_str, CFSTR("6153.40.121.0.1"), kCFCompareNumerically) != kCFCompareLessThan) {
							pmap_sw_asid_off = 0xE6;
							if(CFStringCompare(cf_str, CFSTR("7090.0.0.111.5"), kCFCompareNumerically) != kCFCompareLessThan) {
								pmap_sw_asid_off = 0xDE;
								pvh_high_flags |= PVH_FLAG_PPL_HASHED;
							}
						}
					}
				}
			}
			CFRelease(cf_str);
			if((boot_path = get_boot_path()) != NULL) {
				printf("boot_path: %s\n", boot_path);
				if(pfinder_init_file(&pfinder, boot_path) == KERN_SUCCESS) {
					if(pfinder_init_kbase(&pfinder) == KERN_SUCCESS && (kernproc = pfinder_kernproc(pfinder)) != 0) {
						printf("kernproc: " KADDR_FMT "\n", kernproc);
						if((pv_head_table_ptr = pfinder_pv_head_table_ptr(pfinder)) != 0) {
							printf("pv_head_table_ptr: " KADDR_FMT "\n", pv_head_table_ptr);
							if((const_boot_args = pfinder_const_boot_args(pfinder)) != 0) {
								printf("const_boot_args: " KADDR_FMT "\n", const_boot_args);
								if((lowglo_ptr = pfinder_lowglo_ptr(pfinder)) != 0) {
									printf("lowglo_ptr: " KADDR_FMT "\n", lowglo_ptr);
									ret = KERN_SUCCESS;
								}
							}
						}
					}
					pfinder_term(&pfinder);
				}
				free(boot_path);
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
	} else if(kmem_fd != -1) {
		close(kmem_fd);
	}
	setpriority(PRIO_PROCESS, 0, 0);
}

kern_return_t
golb_init(kaddr_t _kslide, kread_func_t _kread_buf, kwrite_func_t _kwrite_buf) {
	kaddr_t our_task;

	kslide = _kslide;
	if(_kread_buf != NULL && _kwrite_buf != NULL) {
		kread_buf = _kread_buf;
		kwrite_buf = _kwrite_buf;
	} else if(init_tfp0() == KERN_SUCCESS) {
		printf("tfp0: 0x%" PRIX32 "\n", tfp0);
		kread_buf = _kread_buf != NULL ? _kread_buf : kread_buf_tfp0;
		kwrite_buf = _kwrite_buf != NULL ? _kwrite_buf : kwrite_buf_tfp0;
	} else if((kmem_fd = open("/dev/kmem", O_RDWR | O_CLOEXEC)) != -1) {
		kread_buf = _kread_buf != NULL ? _kread_buf : kread_buf_kmem;
		kwrite_buf = _kwrite_buf != NULL ? _kwrite_buf : kwrite_buf_kmem;
	} else {
		return KERN_FAILURE;
	}
	if(setpriority(PRIO_PROCESS, 0, PRIO_MIN) != -1) {
		if(init_arm_pgshift() == KERN_SUCCESS) {
			printf("arm_pgshift: %u\n", arm_pgshift);
			if(pfinder_init_offsets() == KERN_SUCCESS && kread_buf(lowglo_ptr, &lowglo, sizeof(lowglo)) == KERN_SUCCESS && kread_addr(pv_head_table_ptr, &pv_head_table) == KERN_SUCCESS) {
				printf("pv_head_table: " KADDR_FMT "\n", pv_head_table);
				if(kread_buf(const_boot_args, &boot_args, sizeof(boot_args)) == KERN_SUCCESS) {
					printf("virt_base: " KADDR_FMT ", phys_base: " KADDR_FMT ", mem_sz: 0x%" PRIX64 "\n", boot_args.virt_base, boot_args.phys_base, boot_args.mem_sz);
					if(find_task(getpid(), &our_task) == KERN_SUCCESS) {
						kxpacd(&our_task);
						printf("our_task: " KADDR_FMT "\n", our_task);
						if(kread_addr(our_task + task_map_off, &our_map) == KERN_SUCCESS) {
							kxpacd(&our_map);
							printf("our_map: " KADDR_FMT "\n", our_map);
							if(kread_addr(our_map + VM_MAP_PMAP_OFF, &our_pmap) == KERN_SUCCESS) {
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
	} else if(kmem_fd != -1) {
		close(kmem_fd);
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
	kaddr_t vphys, vm_page, virt_off = virt & vm_kernel_page_mask;
	vm_map_entry_t vm_entry;
	vm_page_t m;

	virt -= virt_off;
	if(vm_map_lookup_entry(our_map, virt, &vm_entry) == KERN_SUCCESS && vm_entry.vme_object != 0 && trunc_page_kernel(vm_entry.vme_offset) == 0 && kread_buf(vm_entry.vme_object, &m.vmp_listq.next, sizeof(m.vmp_listq.next)) == KERN_SUCCESS) {
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
	mach_vm_deallocate(mach_task_self(), trunc_page_kernel(ctx.virt), ctx.page_cnt << arm_pgshift);
}

kern_return_t
golb_map(golb_ctx_t *ctx, kaddr_t phys, mach_vm_size_t sz, vm_prot_t prot) {
	kaddr_t phys_off = phys & vm_kernel_page_mask, vm_page, vphys = 0, pv_h, ptep = 0, pte;
	mach_vm_offset_t map_off;
	vm_map_entry_t vm_entry;
	uint32_t flags;
	vm_page_t m;

	phys -= phys_off;
	if((sz = round_page_kernel(sz + phys_off)) != 0 && mach_vm_allocate(mach_task_self(), &ctx->virt, sz, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
		printf("virt: " KADDR_FMT "\n", ctx->virt);
		if(kread_buf(our_map + vm_map_flags_off, &flags, sizeof(flags)) == KERN_SUCCESS) {
			printf("flags: 0x%" PRIX32 "\n", flags);
			flags |= VM_MAP_FLAGS_NO_ZERO_FILL;
			if(kwrite_buf(our_map + vm_map_flags_off, &flags, sizeof(flags)) == KERN_SUCCESS) {
				for(map_off = 0; map_off < sz; map_off += vm_kernel_page_size) {
					*(volatile kaddr_t *)(ctx->virt + map_off) = FAULT_MAGIC;
				}
				flags &= ~VM_MAP_FLAGS_NO_ZERO_FILL;
				if(kwrite_buf(our_map + vm_map_flags_off, &flags, sizeof(flags)) == KERN_SUCCESS && vm_map_lookup_entry(our_map, ctx->virt, &vm_entry) == KERN_SUCCESS && vm_entry.vme_object != 0 && trunc_page_kernel(vm_entry.vme_offset) == 0 && vm_entry.links.end - ctx->virt >= sz && kread_buf(vm_entry.vme_object, &m.vmp_listq.next, sizeof(m.vmp_listq.next)) == KERN_SUCCESS && (ctx->pages = calloc(sz >> arm_pgshift, sizeof(ctx->pages[0]))) != NULL) {
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
							if(vphys < boot_args.phys_base || vphys >= trunc_page_kernel(boot_args.phys_base + boot_args.mem_sz) || kread_addr(pv_head_table + ((vphys - boot_args.phys_base) >> vm_kernel_page_shift) * sizeof(kaddr_t), &pv_h) != KERN_SUCCESS) {
								break;
							}
							printf("pv_h: " KADDR_FMT "\n", pv_h);
							if((pv_h & PVH_TYPE_MASK) != PVH_TYPE_PTEP) {
								break;
							}
							ptep = (pv_h & PVH_LIST_MASK) | pvh_high_flags;
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
