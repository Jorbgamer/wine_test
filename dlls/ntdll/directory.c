/*
 * NTDLL directory functions
 *
 * Copyright 1993 Erik Bos
 * Copyright 2003 Eric Pouech
 * Copyright 1996, 2004 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#include <sys/stat.h>
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_LINUX_IOCTL_H
#include <linux/ioctl.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#include <time.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winreg.h"
#include "ntstatus.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/unicode.h"
#include "wine/server.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(file);

/* Define the VFAT ioctl to get both short and long file names */
/* FIXME: is it possible to get this to work on other systems? */
#ifdef linux
/* We want the real kernel dirent structure, not the libc one */
typedef struct
{
    long d_ino;
    long d_off;
    unsigned short d_reclen;
    char d_name[256];
} KERNEL_DIRENT;

#define VFAT_IOCTL_READDIR_BOTH  _IOR('r', 1, KERNEL_DIRENT [2] )

#ifndef O_DIRECTORY
# define O_DIRECTORY 0200000 /* must be directory */
#endif

/* Using the same seekdir value across multiple directories is not portable,  */
/* but it works on Linux, and it's a major performance gain so we want to use */
/* it if possible. */
/* FIXME: do some sort of runtime check instead */
#define USE_SEEKDIR

#else   /* linux */
#undef VFAT_IOCTL_READDIR_BOTH  /* just in case... */
#undef USE_SEEKDIR
#endif  /* linux */

#define IS_OPTION_TRUE(ch) ((ch) == 'y' || (ch) == 'Y' || (ch) == 't' || (ch) == 'T' || (ch) == '1')
#define IS_SEPARATOR(ch)   ((ch) == '\\' || (ch) == '/')

#define INVALID_NT_CHARS   '*','?','<','>','|','"'
#define INVALID_DOS_CHARS  INVALID_NT_CHARS,'+','=',',',';','[',']',' ','\345'

#define MAX_DIR_ENTRY_LEN 255  /* max length of a directory entry in chars */

static int show_dir_symlinks = -1;
static int show_dot_files;

/* at some point we may want to allow Winelib apps to set this */
static const int is_case_sensitive = FALSE;

static CRITICAL_SECTION dir_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &dir_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { 0, (DWORD)(__FILE__ ": dir_section") }
};
static CRITICAL_SECTION dir_section = { &critsect_debug, -1, 0, 0, 0, 0 };


/***********************************************************************
 *           seekdir_wrapper
 *
 * Wrapper for supporting seekdir across multiple directory objects.
 */
static inline void seekdir_wrapper( DIR *dir, off_t pos )
{
#ifdef USE_SEEKDIR
    seekdir( dir, pos );
#else
    while (pos-- > 0) if (!readdir( dir )) break;
#endif
}

/***********************************************************************
 *           telldir_wrapper
 *
 * Wrapper for supporting telldir across multiple directory objects.
 */
static inline off_t telldir_wrapper( DIR *dir, off_t pos, int count )
{
#ifdef USE_SEEKDIR
    return telldir( dir );
#else
    return pos + count;
#endif
}


/***********************************************************************
 *           get_default_com_device
 *
 * Return the default device to use for serial ports.
 */
static char *get_default_com_device( int num )
{
    char *ret = NULL;

    if (!num || num > 9) return ret;
#ifdef linux
    ret = RtlAllocateHeap( GetProcessHeap(), 0, sizeof("/dev/ttyS0") );
    if (ret)
    {
        strcpy( ret, "/dev/ttyS0" );
        ret[strlen(ret) - 1] = '0' + num - 1;
    }
#else
    FIXME( "no known default for device com%d\n", num );
#endif
    return ret;
}


/***********************************************************************
 *           get_default_lpt_device
 *
 * Return the default device to use for parallel ports.
 */
static char *get_default_lpt_device( int num )
{
    char *ret = NULL;

    if (!num || num > 9) return ret;
#ifdef linux
    ret = RtlAllocateHeap( GetProcessHeap(), 0, sizeof("/dev/lp0") );
    if (ret)
    {
        strcpy( ret, "/dev/lp0" );
        ret[strlen(ret) - 1] = '0' + num - 1;
    }
#else
    FIXME( "no known default for device lpt%d\n", num );
#endif
    return ret;
}


/***********************************************************************
 *           parse_mount_entries
 *
 * Parse mount entries looking for a given device. Helper for get_default_drive_device.
 */
#ifdef linux
static char *parse_mount_entries( FILE *f, dev_t dev, ino_t ino )
{
    struct mntent *entry;
    struct stat st;
    char *device;

    while ((entry = getmntent( f )))
    {
        if (stat( entry->mnt_dir, &st ) == -1) continue;
        if (st.st_dev != dev || st.st_ino != ino) continue;
        if (!strcmp( entry->mnt_type, "supermount" ))
        {
            if ((device = strstr( entry->mnt_opts, "dev=" )))
            {
                char *p = strchr( device + 4, ',' );
                if (p) *p = 0;
                return device + 4;
            }
        }
        else
            return entry->mnt_fsname;
    }
    return NULL;
}
#endif

/***********************************************************************
 *           get_default_drive_device
 *
 * Return the default device to use for a given drive mount point.
 */
static char *get_default_drive_device( const char *root )
{
    char *ret = NULL;

#ifdef linux
    FILE *f;
    char *device = NULL;
    int fd, res = -1;
    struct stat st;

    /* try to open it first to force it to get mounted */
    if ((fd = open( root, O_RDONLY | O_DIRECTORY )) != -1)
    {
        res = fstat( fd, &st );
        close( fd );
    }
    /* now try normal stat just in case */
    if (res == -1) res = stat( root, &st );
    if (res == -1) return NULL;

    RtlEnterCriticalSection( &dir_section );

    if ((f = fopen( "/etc/mtab", "r" )))
    {
        device = parse_mount_entries( f, st.st_dev, st.st_ino );
        endmntent( f );
    }
    /* look through fstab too in case it's not mounted (for instance if it's an audio CD) */
    if (!device && (f = fopen( "/etc/fstab", "r" )))
    {
        device = parse_mount_entries( f, st.st_dev, st.st_ino );
        endmntent( f );
    }
    if (device)
    {
        ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(device) + 1 );
        if (ret) strcpy( ret, device );
    }
    RtlLeaveCriticalSection( &dir_section );
#elif defined(__APPLE__)
    struct statfs *mntStat;
    struct stat st;
    int i;
    int mntSize;
    dev_t dev;
    ino_t ino;
    static const char path_bsd_device[] = "/dev/disk";
    int res;

    res = stat( root, &st );
    if (res == -1) return NULL;

    dev = st.st_dev;
    ino = st.st_ino;

    RtlEnterCriticalSection( &dir_section );

    mntSize = getmntinfo(&mntStat, MNT_NOWAIT);

    for (i = 0; i < mntSize && !ret; i++)
    {
        if (stat(mntStat[i].f_mntonname, &st ) == -1) continue;
        if (st.st_dev != dev || st.st_ino != ino) continue;

        /* FIXME add support for mounted network drive */
        if ( strncmp(mntStat[i].f_mntfromname, path_bsd_device, strlen(path_bsd_device)) == 0)
        {
            /* set return value to the corresponding raw BSD node */
            ret = RtlAllocateHeap( GetProcessHeap(), 0, strlen(mntStat[i].f_mntfromname) + 2 /* 2 : r and \0 */ );
            if (ret)
            {
                strcpy(ret, "/dev/r");
                strcat(ret, mntStat[i].f_mntfromname+sizeof("/dev/")-1);
            }
        }
    }
    RtlLeaveCriticalSection( &dir_section );
#else
    static int warned;
    if (!warned++) FIXME( "auto detection of DOS devices not supported on this platform\n" );
#endif
    return ret;
}


/***********************************************************************
 *           init_options
 *
 * Initialize the show_dir_symlinks and show_dot_files options.
 */
static void init_options(void)
{
    static const WCHAR WineW[] = {'M','a','c','h','i','n','e','\\',
                                  'S','o','f','t','w','a','r','e','\\',
                                  'W','i','n','e','\\','W','i','n','e','\\',
                                  'C','o','n','f','i','g','\\','W','i','n','e',0};
    static const WCHAR ShowDotFilesW[] = {'S','h','o','w','D','o','t','F','i','l','e','s',0};
    static const WCHAR ShowDirSymlinksW[] = {'S','h','o','w','D','i','r','S','y','m','l','i','n','k','s',0};
    char tmp[80];
    HKEY hkey;
    DWORD dummy;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;

    show_dot_files = show_dir_symlinks = 0;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    RtlInitUnicodeString( &nameW, WineW );

    if (!NtOpenKey( &hkey, KEY_ALL_ACCESS, &attr ))
    {
        RtlInitUnicodeString( &nameW, ShowDotFilesW );
        if (!NtQueryValueKey( hkey, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &dummy ))
        {
            WCHAR *str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;
            show_dot_files = IS_OPTION_TRUE( str[0] );
        }
        RtlInitUnicodeString( &nameW, ShowDirSymlinksW );
        if (!NtQueryValueKey( hkey, &nameW, KeyValuePartialInformation, tmp, sizeof(tmp), &dummy ))
        {
            WCHAR *str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)tmp)->Data;
            show_dir_symlinks = IS_OPTION_TRUE( str[0] );
        }
        NtClose( hkey );
    }
}


/***********************************************************************
 *           DIR_is_hidden_file
 *
 * Check if the specified file should be hidden based on its name and the show dot files option.
 */
BOOL DIR_is_hidden_file( const UNICODE_STRING *name )
{
    WCHAR *p, *end;

    if (show_dir_symlinks == -1) init_options();
    if (show_dot_files) return FALSE;

    end = p = name->Buffer + name->Length/sizeof(WCHAR);
    while (p > name->Buffer && IS_SEPARATOR(p[-1])) p--;
    while (p > name->Buffer && !IS_SEPARATOR(p[-1])) p--;
    if (p == end || *p != '.') return FALSE;
    /* make sure it isn't '.' or '..' */
    if (p + 1 == end) return FALSE;
    if (p[1] == '.' && p + 2 == end) return FALSE;
    return TRUE;
}


/***********************************************************************
 *           hash_short_file_name
 *
 * Transform a Unix file name into a hashed DOS name. If the name is a valid
 * DOS name, it is converted to upper-case; otherwise it is replaced by a
 * hashed version that fits in 8.3 format.
 * 'buffer' must be at least 12 characters long.
 * Returns length of short name in bytes; short name is NOT null-terminated.
 */
static ULONG hash_short_file_name( const UNICODE_STRING *name, LPWSTR buffer )
{
    static const WCHAR invalid_chars[] = { INVALID_DOS_CHARS,'~','.',0 };
    static const char hash_chars[32] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";

    LPCWSTR p, ext, end = name->Buffer + name->Length / sizeof(WCHAR);
    LPWSTR dst;
    unsigned short hash;
    int i;

    /* Compute the hash code of the file name */
    /* If you know something about hash functions, feel free to */
    /* insert a better algorithm here... */
    if (!is_case_sensitive)
    {
        for (p = name->Buffer, hash = 0xbeef; p < end - 1; p++)
            hash = (hash<<3) ^ (hash>>5) ^ tolowerW(*p) ^ (tolowerW(p[1]) << 8);
        hash = (hash<<3) ^ (hash>>5) ^ tolowerW(*p); /* Last character */
    }
    else
    {
        for (p = name->Buffer, hash = 0xbeef; p < end - 1; p++)
            hash = (hash << 3) ^ (hash >> 5) ^ *p ^ (p[1] << 8);
        hash = (hash << 3) ^ (hash >> 5) ^ *p;  /* Last character */
    }

    /* Find last dot for start of the extension */
    for (p = name->Buffer + 1, ext = NULL; p < end - 1; p++) if (*p == '.') ext = p;

    /* Copy first 4 chars, replacing invalid chars with '_' */
    for (i = 4, p = name->Buffer, dst = buffer; i > 0; i--, p++)
    {
        if (p == end || p == ext) break;
        *dst++ = strchrW( invalid_chars, *p ) ? '_' : toupperW(*p);
    }
    /* Pad to 5 chars with '~' */
    while (i-- >= 0) *dst++ = '~';

    /* Insert hash code converted to 3 ASCII chars */
    *dst++ = hash_chars[(hash >> 10) & 0x1f];
    *dst++ = hash_chars[(hash >> 5) & 0x1f];
    *dst++ = hash_chars[hash & 0x1f];

    /* Copy the first 3 chars of the extension (if any) */
    if (ext)
    {
        *dst++ = '.';
        for (i = 3, ext++; (i > 0) && ext < end; i--, ext++)
            *dst++ = strchrW( invalid_chars, *ext ) ? '_' : toupperW(*ext);
    }
    return dst - buffer;
}


/***********************************************************************
 *           match_filename
 *
 * Check a long file name against a mask.
 *
 * Tests (done in W95 DOS shell - case insensitive):
 * *.txt			test1.test.txt				*
 * *st1*			test1.txt				*
 * *.t??????.t*			test1.ta.tornado.txt			*
 * *tornado*			test1.ta.tornado.txt			*
 * t*t				test1.ta.tornado.txt			*
 * ?est*			test1.txt				*
 * ?est???			test1.txt				-
 * *test1.txt*			test1.txt				*
 * h?l?o*t.dat			hellothisisatest.dat			*
 */
static BOOLEAN match_filename( const UNICODE_STRING *name_str, const UNICODE_STRING *mask_str )
{
    int mismatch;
    const WCHAR *name = name_str->Buffer;
    const WCHAR *mask = mask_str->Buffer;
    const WCHAR *name_end = name + name_str->Length / sizeof(WCHAR);
    const WCHAR *mask_end = mask + mask_str->Length / sizeof(WCHAR);
    const WCHAR *lastjoker = NULL;
    const WCHAR *next_to_retry = NULL;

    TRACE("(%s, %s)\n", debugstr_us(name_str), debugstr_us(mask_str));

    while (name < name_end && mask < mask_end)
    {
        switch(*mask)
        {
        case '*':
            mask++;
            while (mask < mask_end && *mask == '*') mask++;  /* Skip consecutive '*' */
            if (mask == mask_end) return TRUE; /* end of mask is all '*', so match */
            lastjoker = mask;

            /* skip to the next match after the joker(s) */
            if (is_case_sensitive)
                while (name < name_end && (*name != *mask)) name++;
            else
                while (name < name_end && (toupperW(*name) != toupperW(*mask))) name++;
            next_to_retry = name;
            break;
        case '?':
            mask++;
            name++;
            break;
        default:
            if (is_case_sensitive) mismatch = (*mask != *name);
            else mismatch = (toupperW(*mask) != toupperW(*name));

            if (!mismatch)
            {
                mask++;
                name++;
                if (mask == mask_end)
                {
                    if (name == name_end) return TRUE;
                    if (lastjoker) mask = lastjoker;
                }
            }
            else /* mismatch ! */
            {
                if (lastjoker) /* we had an '*', so we can try unlimitedly */
                {
                    mask = lastjoker;

                    /* this scan sequence was a mismatch, so restart
                     * 1 char after the first char we checked last time */
                    next_to_retry++;
                    name = next_to_retry;
                }
                else return FALSE; /* bad luck */
            }
            break;
        }
    }
    while (mask < mask_end && ((*mask == '.') || (*mask == '*')))
        mask++;  /* Ignore trailing '.' or '*' in mask */
    return (name == name_end && mask == mask_end);
}


/***********************************************************************
 *           append_entry
 *
 * helper for NtQueryDirectoryFile
 */
static FILE_BOTH_DIR_INFORMATION *append_entry( void *info_ptr, ULONG *pos, ULONG max_length,
                                                const char *long_name, const char *short_name,
                                                const UNICODE_STRING *mask )
{
    FILE_BOTH_DIR_INFORMATION *info;
    int i, long_len, short_len, total_len;
    struct stat st;
    WCHAR long_nameW[MAX_DIR_ENTRY_LEN];
    WCHAR short_nameW[12];
    UNICODE_STRING str;

    long_len = ntdll_umbstowcs( 0, long_name, strlen(long_name), long_nameW, MAX_DIR_ENTRY_LEN );
    if (long_len == -1) return NULL;

    str.Buffer = long_nameW;
    str.Length = long_len * sizeof(WCHAR);
    str.MaximumLength = sizeof(long_nameW);

    if (short_name)
    {
        short_len = ntdll_umbstowcs( 0, short_name, strlen(short_name),
                                     short_nameW, sizeof(short_nameW) / sizeof(WCHAR) );
        if (short_len == -1) short_len = sizeof(short_nameW) / sizeof(WCHAR);
    }
    else  /* generate a short name if necessary */
    {
        BOOLEAN spaces;

        short_len = 0;
        if (!RtlIsNameLegalDOS8Dot3( &str, NULL, &spaces ) || spaces)
            short_len = hash_short_file_name( &str, short_nameW );
    }

    TRACE( "long %s short %s mask %s\n",
           debugstr_us(&str), debugstr_wn(short_nameW, short_len), debugstr_us(mask) );

    if (mask && !match_filename( &str, mask ))
    {
        if (!short_len) return NULL;  /* no short name to match */
        str.Buffer = short_nameW;
        str.Length = short_len * sizeof(WCHAR);
        str.MaximumLength = sizeof(short_nameW);
        if (!match_filename( &str, mask )) return NULL;
    }

    total_len = (sizeof(*info) - sizeof(info->FileName) + long_len*sizeof(WCHAR) + 3) & ~3;
    info = (FILE_BOTH_DIR_INFORMATION *)((char *)info_ptr + *pos);

    if (*pos + total_len > max_length) total_len = max_length - *pos;

    if (lstat( long_name, &st ) == -1) return NULL;
    if (S_ISLNK( st.st_mode ))
    {
        if (stat( long_name, &st ) == -1) return NULL;
        if (S_ISDIR( st.st_mode ) && !show_dir_symlinks) return NULL;
    }

    info->NextEntryOffset = total_len;
    info->FileIndex = 0;  /* NTFS always has 0 here, so let's not bother with it */

    RtlSecondsSince1970ToTime( st.st_mtime, &info->CreationTime );
    RtlSecondsSince1970ToTime( st.st_mtime, &info->LastWriteTime );
    RtlSecondsSince1970ToTime( st.st_atime, &info->LastAccessTime );
    RtlSecondsSince1970ToTime( st.st_ctime, &info->ChangeTime );

    if (S_ISDIR(st.st_mode))
    {
        info->EndOfFile.QuadPart = info->AllocationSize.QuadPart = 0;
        info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    }
    else
    {
        info->EndOfFile.QuadPart = st.st_size;
        info->AllocationSize.QuadPart = (ULONGLONG)st.st_blocks * 512;
        info->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    }

    if (!(st.st_mode & S_IWUSR))
        info->FileAttributes |= FILE_ATTRIBUTE_READONLY;

    if (!show_dot_files && long_name[0] == '.' && long_name[1] && (long_name[1] != '.' || long_name[2]))
        info->FileAttributes |= FILE_ATTRIBUTE_HIDDEN;

    info->EaSize = 0; /* FIXME */
    info->ShortNameLength = short_len * sizeof(WCHAR);
    for (i = 0; i < short_len; i++) info->ShortName[i] = toupperW(short_nameW[i]);
    info->FileNameLength = long_len * sizeof(WCHAR);
    memcpy( info->FileName, long_nameW,
            min( info->FileNameLength, total_len-sizeof(*info)+sizeof(info->FileName) ));

    *pos += total_len;
    return info;
}


/******************************************************************************
 *  NtQueryDirectoryFile	[NTDLL.@]
 *  ZwQueryDirectoryFile	[NTDLL.@]
 */
NTSTATUS WINAPI NtQueryDirectoryFile( HANDLE handle, HANDLE event,
                                      PIO_APC_ROUTINE apc_routine, PVOID apc_context,
                                      PIO_STATUS_BLOCK io,
                                      PVOID buffer, ULONG length,
                                      FILE_INFORMATION_CLASS info_class,
                                      BOOLEAN single_entry,
                                      PUNICODE_STRING mask,
                                      BOOLEAN restart_scan )
{
    int cwd, fd;
    FILE_BOTH_DIR_INFORMATION *info, *last_info = NULL;
    static const unsigned int max_dir_info_size = sizeof(*info) + (MAX_DIR_ENTRY_LEN-1) * sizeof(WCHAR);

    TRACE("(%p %p %p %p %p %p 0x%08lx 0x%08x 0x%08x %s 0x%08x\n",
          handle, event, apc_routine, apc_context, io, buffer,
          length, info_class, single_entry, debugstr_us(mask),
          restart_scan);

    if (length < sizeof(*info)) return STATUS_INFO_LENGTH_MISMATCH;

    if (event || apc_routine)
    {
        FIXME( "Unsupported yet option\n" );
        return io->u.Status = STATUS_NOT_IMPLEMENTED;
    }
    if (info_class != FileBothDirectoryInformation)
    {
        FIXME( "Unsupported file info class %d\n", info_class );
        return io->u.Status = STATUS_NOT_IMPLEMENTED;
    }

    if ((io->u.Status = wine_server_handle_to_fd( handle, GENERIC_READ,
                                                  &fd, NULL, NULL )) != STATUS_SUCCESS)
        return io->u.Status;

    io->Information = 0;

    RtlEnterCriticalSection( &dir_section );

    if (show_dir_symlinks == -1) init_options();

    if ((cwd = open(".", O_RDONLY)) != -1 && fchdir( fd ) != -1)
    {
        off_t old_pos = 0;

#ifdef VFAT_IOCTL_READDIR_BOTH
        KERNEL_DIRENT de[2];

        io->u.Status = STATUS_SUCCESS;

        /* Check if the VFAT ioctl is supported on this directory */

        if (restart_scan) lseek( fd, 0, SEEK_SET );
        else old_pos = lseek( fd, 0, SEEK_CUR );

        if (ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)de ) != -1)
        {
            if (length < max_dir_info_size)  /* we may have to return a partial entry here */
            {
                for (;;)
                {
                    if (!de[0].d_reclen) break;
                    if (de[1].d_name[0])
                        info = append_entry( buffer, &io->Information, length,
                                             de[1].d_name, de[0].d_name, mask );
                    else
                        info = append_entry( buffer, &io->Information, length,
                                             de[0].d_name, NULL, mask );
                    if (info)
                    {
                        last_info = info;
                        if ((char *)info->FileName + info->FileNameLength > (char *)buffer + length)
                        {
                            io->u.Status = STATUS_BUFFER_OVERFLOW;
                            lseek( fd, old_pos, SEEK_SET );  /* restore pos to previous entry */
                        }
                        break;
                    }
                    old_pos = lseek( fd, 0, SEEK_CUR );
                    if (ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)de ) == -1) break;
                }
            }
            else  /* we'll only return full entries, no need to worry about overflow */
            {
                for (;;)
                {
                    if (!de[0].d_reclen) break;
                    if (de[1].d_name[0])
                        info = append_entry( buffer, &io->Information, length,
                                             de[1].d_name, de[0].d_name, mask );
                    else
                        info = append_entry( buffer, &io->Information, length,
                                             de[0].d_name, NULL, mask );
                    if (info)
                    {
                        last_info = info;
                        if (single_entry) break;
                        /* check if we still have enough space for the largest possible entry */
                        if (io->Information + max_dir_info_size > length) break;
                    }
                    if (ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)de ) == -1) break;
                }
            }
        }
        else if (errno != ENOENT)
#endif  /* VFAT_IOCTL_READDIR_BOTH */
        {
            DIR *dir;
            struct dirent *de;

            if (!(dir = opendir( "." )))
            {
                io->u.Status = FILE_GetNtStatus();
                goto done;
            }
            if (!restart_scan)
            {
                old_pos = lseek( fd, 0, SEEK_CUR );
                seekdir_wrapper( dir, old_pos );
            }
            io->u.Status = STATUS_SUCCESS;

            if (length < max_dir_info_size)  /* we may have to return a partial entry here */
            {
                while ((de = readdir( dir )))
                {
                    info = append_entry( buffer, &io->Information, length,
                                         de->d_name, NULL, mask );
                    if (info)
                    {
                        last_info = info;
                        if ((char *)info->FileName + info->FileNameLength > (char *)buffer + length)
                            io->u.Status = STATUS_BUFFER_OVERFLOW;
                        else
                            old_pos = telldir_wrapper( dir, old_pos, 1 );
                        break;
                    }
                    old_pos = telldir_wrapper( dir, old_pos, 1 );
                }
            }
            else  /* we'll only return full entries, no need to worry about overflow */
            {
                int count = 0;
                while ((de = readdir( dir )))
                {
                    count++;
                    info = append_entry( buffer, &io->Information, length,
                                         de->d_name, NULL, mask );
                    if (info)
                    {
                        last_info = info;
                        if (single_entry) break;
                        /* check if we still have enough space for the largest possible entry */
                        if (io->Information + max_dir_info_size > length) break;
                    }
                }
                old_pos = telldir_wrapper( dir, old_pos, count );
            }
            lseek( fd, old_pos, SEEK_SET );  /* store dir offset as filepos for fd */
            closedir( dir );
        }

        if (last_info) last_info->NextEntryOffset = 0;
        else io->u.Status = restart_scan ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;

    done:
        if (fchdir( cwd ) == -1) chdir( "/" );
    }
    else io->u.Status = FILE_GetNtStatus();

    RtlLeaveCriticalSection( &dir_section );

    wine_server_release_fd( handle, fd );
    if (cwd != -1) close( cwd );
    TRACE( "=> %lx (%ld)\n", io->u.Status, io->Information );
    return io->u.Status;
}


/***********************************************************************
 *           find_file_in_dir
 *
 * Find a file in a directory the hard way, by doing a case-insensitive search.
 * The file found is appended to unix_name at pos.
 * There must be at least MAX_DIR_ENTRY_LEN+2 chars available at pos.
 */
static NTSTATUS find_file_in_dir( char *unix_name, int pos, const WCHAR *name, int length,
                                  int check_case )
{
    WCHAR buffer[MAX_DIR_ENTRY_LEN];
    UNICODE_STRING str;
    BOOLEAN spaces;
    DIR *dir;
    struct dirent *de;
    struct stat st;
    int ret, used_default, is_name_8_dot_3;

    /* try a shortcut for this directory */

    unix_name[pos++] = '/';
    ret = ntdll_wcstoumbs( 0, name, length, unix_name + pos, MAX_DIR_ENTRY_LEN,
                           NULL, &used_default );
    /* if we used the default char, the Unix name won't round trip properly back to Unicode */
    /* so it cannot match the file we are looking for */
    if (ret >= 0 && !used_default)
    {
        unix_name[pos + ret] = 0;
        if (!stat( unix_name, &st )) return STATUS_SUCCESS;
    }
    if (check_case) goto not_found;  /* we want an exact match */

    if (pos > 1) unix_name[pos - 1] = 0;
    else unix_name[1] = 0;  /* keep the initial slash */

    /* check if it fits in 8.3 so that we don't look for short names if we won't need them */

    str.Buffer = (WCHAR *)name;
    str.Length = length * sizeof(WCHAR);
    str.MaximumLength = str.Length;
    is_name_8_dot_3 = RtlIsNameLegalDOS8Dot3( &str, NULL, &spaces ) && !spaces;

    /* now look for it through the directory */

#ifdef VFAT_IOCTL_READDIR_BOTH
    if (is_name_8_dot_3)
    {
        int fd = open( unix_name, O_RDONLY | O_DIRECTORY );
        if (fd != -1)
        {
            KERNEL_DIRENT de[2];

            if (ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)de ) != -1)
            {
                unix_name[pos - 1] = '/';
                for (;;)
                {
                    if (!de[0].d_reclen) break;

                    if (de[1].d_name[0])
                    {
                        ret = ntdll_umbstowcs( 0, de[1].d_name, strlen(de[1].d_name),
                                               buffer, MAX_DIR_ENTRY_LEN );
                        if (ret == length && !memicmpW( buffer, name, length))
                        {
                            strcpy( unix_name + pos, de[1].d_name );
                            close( fd );
                            return STATUS_SUCCESS;
                        }
                    }
                    ret = ntdll_umbstowcs( 0, de[0].d_name, strlen(de[0].d_name),
                                           buffer, MAX_DIR_ENTRY_LEN );
                    if (ret == length && !memicmpW( buffer, name, length))
                    {
                        strcpy( unix_name + pos,
                                de[1].d_name[0] ? de[1].d_name : de[0].d_name );
                        close( fd );
                        return STATUS_SUCCESS;
                    }
                    if (ioctl( fd, VFAT_IOCTL_READDIR_BOTH, (long)de ) == -1)
                    {
                        close( fd );
                        goto not_found;
                    }
                }
            }
            close( fd );
        }
        /* fall through to normal handling */
    }
#endif /* VFAT_IOCTL_READDIR_BOTH */

    if (!(dir = opendir( unix_name )))
    {
        if (errno == ENOENT) return STATUS_OBJECT_PATH_NOT_FOUND;
        else return FILE_GetNtStatus();
    }
    unix_name[pos - 1] = '/';
    str.Buffer = buffer;
    str.MaximumLength = sizeof(buffer);
    while ((de = readdir( dir )))
    {
        ret = ntdll_umbstowcs( 0, de->d_name, strlen(de->d_name), buffer, MAX_DIR_ENTRY_LEN );
        if (ret == length && !memicmpW( buffer, name, length ))
        {
            strcpy( unix_name + pos, de->d_name );
            closedir( dir );
            return STATUS_SUCCESS;
        }

        if (!is_name_8_dot_3) continue;

        str.Length = ret * sizeof(WCHAR);
        if (!RtlIsNameLegalDOS8Dot3( &str, NULL, &spaces ) || spaces)
        {
            WCHAR short_nameW[12];
            ret = hash_short_file_name( &str, short_nameW );
            if (ret == length && !memicmpW( short_nameW, name, length ))
            {
                strcpy( unix_name + pos, de->d_name );
                closedir( dir );
                return STATUS_SUCCESS;
            }
        }
    }
    closedir( dir );
    goto not_found;  /* avoid warning */

not_found:
    unix_name[pos - 1] = 0;
    return STATUS_OBJECT_PATH_NOT_FOUND;
}


/******************************************************************************
 *           get_dos_device
 *
 * Get the Unix path of a DOS device.
 */
static NTSTATUS get_dos_device( const WCHAR *name, UINT name_len, ANSI_STRING *unix_name_ret )
{
    const char *config_dir = wine_get_config_dir();
    struct stat st;
    char *unix_name, *new_name, *dev;
    unsigned int i;
    int unix_len;

    /* make sure the device name is ASCII */
    for (i = 0; i < name_len; i++)
        if (name[i] <= 32 || name[i] >= 127) return STATUS_OBJECT_NAME_NOT_FOUND;

    unix_len = strlen(config_dir) + sizeof("/dosdevices/") + name_len + 1;

    if (!(unix_name = RtlAllocateHeap( GetProcessHeap(), 0, unix_len )))
        return STATUS_NO_MEMORY;

    strcpy( unix_name, config_dir );
    strcat( unix_name, "/dosdevices/" );
    dev = unix_name + strlen(unix_name);

    for (i = 0; i < name_len; i++) dev[i] = (char)tolowerW(name[i]);
    dev[i] = 0;

    /* special case for drive devices */
    if (name_len == 2 && dev[1] == ':')
    {
        dev[i++] = ':';
        dev[i] = 0;
    }

    for (;;)
    {
        if (!stat( unix_name, &st ))
        {
            TRACE( "%s -> %s\n", debugstr_wn(name,name_len), debugstr_a(unix_name) );
            unix_name_ret->Buffer = unix_name;
            unix_name_ret->Length = strlen(unix_name);
            unix_name_ret->MaximumLength = unix_len;
            return STATUS_SUCCESS;
        }
        if (!dev) break;

        /* now try some defaults for it */
        if (!strcmp( dev, "aux" ))
        {
            strcpy( dev, "com1" );
            continue;
        }
        if (!strcmp( dev, "prn" ))
        {
            strcpy( dev, "lpt1" );
            continue;
        }
        if (!strcmp( dev, "nul" ))
        {
            strcpy( unix_name, "/dev/null" );
            dev = NULL; /* last try */
            continue;
        }

        new_name = NULL;
        if (dev[1] == ':' && dev[2] == ':')  /* drive device */
        {
            dev[2] = 0;  /* remove last ':' to get the drive mount point symlink */
            new_name = get_default_drive_device( unix_name );
        }
        else if (!strncmp( dev, "com", 3 )) new_name = get_default_com_device( dev[3] - '0' );
        else if (!strncmp( dev, "lpt", 3 )) new_name = get_default_lpt_device( dev[3] - '0' );

        if (!new_name) break;

        RtlFreeHeap( GetProcessHeap(), 0, unix_name );
        unix_name = new_name;
        unix_len = strlen(unix_name) + 1;
        dev = NULL; /* last try */
    }
    RtlFreeHeap( GetProcessHeap(), 0, unix_name );
    return STATUS_OBJECT_NAME_NOT_FOUND;
}


/* return the length of the DOS namespace prefix if any */
static inline int get_dos_prefix_len( const UNICODE_STRING *name )
{
    static const WCHAR nt_prefixW[] = {'\\','?','?','\\'};
    static const WCHAR dosdev_prefixW[] = {'\\','D','o','s','D','e','v','i','c','e','s','\\'};

    if (name->Length > sizeof(nt_prefixW) &&
        !memcmp( name->Buffer, nt_prefixW, sizeof(nt_prefixW) ))
        return sizeof(nt_prefixW) / sizeof(WCHAR);

    if (name->Length > sizeof(dosdev_prefixW) &&
        !memicmpW( name->Buffer, dosdev_prefixW, sizeof(dosdev_prefixW)/sizeof(WCHAR) ))
        return sizeof(dosdev_prefixW) / sizeof(WCHAR);

    return 0;
}


/******************************************************************************
 *           wine_nt_to_unix_file_name  (NTDLL.@) Not a Windows API
 *
 * Convert a file name from NT namespace to Unix namespace.
 *
 * If disposition is not FILE_OPEN or FILE_OVERWRITTE, the last path
 * element doesn't have to exist; in that case STATUS_NO_SUCH_FILE is
 * returned, but the unix name is still filled in properly.
 */
NTSTATUS wine_nt_to_unix_file_name( const UNICODE_STRING *nameW, ANSI_STRING *unix_name_ret,
                                    UINT disposition, BOOLEAN check_case )
{
    static const WCHAR uncW[] = {'U','N','C','\\'};
    static const WCHAR invalid_charsW[] = { INVALID_NT_CHARS, 0 };

    NTSTATUS status = STATUS_SUCCESS;
    const char *config_dir = wine_get_config_dir();
    const WCHAR *name, *p;
    struct stat st;
    char *unix_name;
    int pos, ret, name_len, unix_len, used_default;

    name     = nameW->Buffer;
    name_len = nameW->Length / sizeof(WCHAR);

    if (!name_len || !IS_SEPARATOR(name[0])) return STATUS_OBJECT_PATH_SYNTAX_BAD;

    if ((pos = get_dos_prefix_len( nameW )))
    {
        BOOLEAN is_unc = FALSE;

        name += pos;
        name_len -= pos;

        /* check for UNC prefix */
        if (name_len > 4 && !memicmpW( name, uncW, 4 ))
        {
            name += 3;
            name_len -= 3;
            is_unc = TRUE;
        }
        else
        {
            /* check for a drive letter with path */
            if (name_len < 3 || !isalphaW(name[0]) || name[1] != ':' || !IS_SEPARATOR(name[2]))
            {
                /* not a drive with path, try other DOS devices */
                return get_dos_device( name, name_len, unix_name_ret );
            }
            name += 2;  /* skip drive letter */
            name_len -= 2;
        }

        /* check for invalid characters */
        for (p = name; p < name + name_len; p++)
            if (*p < 32 || strchrW( invalid_charsW, *p )) return STATUS_OBJECT_NAME_INVALID;

        unix_len = ntdll_wcstoumbs( 0, name, name_len, NULL, 0, NULL, NULL );
        unix_len += MAX_DIR_ENTRY_LEN + 3;
        unix_len += strlen(config_dir) + sizeof("/dosdevices/") + 3;
        if (!(unix_name = RtlAllocateHeap( GetProcessHeap(), 0, unix_len )))
            return STATUS_NO_MEMORY;
        strcpy( unix_name, config_dir );
        strcat( unix_name, "/dosdevices/" );
        pos = strlen(unix_name);
        if (is_unc)
        {
            strcpy( unix_name + pos, "unc" );
            pos += 3;
        }
        else
        {
            unix_name[pos++] = tolowerW( name[-2] );
            unix_name[pos++] = ':';
            unix_name[pos] = 0;
        }
    }
    else  /* no DOS prefix, assume NT native name, map directly to Unix */
    {
        if (!name_len || !IS_SEPARATOR(name[0])) return STATUS_OBJECT_NAME_INVALID;
        unix_len = ntdll_wcstoumbs( 0, name, name_len, NULL, 0, NULL, NULL );
        unix_len += MAX_DIR_ENTRY_LEN + 3;
        if (!(unix_name = RtlAllocateHeap( GetProcessHeap(), 0, unix_len )))
            return STATUS_NO_MEMORY;
        pos = 0;
    }

    /* try a shortcut first */

    ret = ntdll_wcstoumbs( 0, name, name_len, unix_name + pos, unix_len - pos - 1,
                           NULL, &used_default );

    while (name_len && IS_SEPARATOR(*name))
    {
        name++;
        name_len--;
    }

    if (ret > 0 && !used_default)  /* if we used the default char the name didn't convert properly */
    {
        char *p;
        unix_name[pos + ret] = 0;
        for (p = unix_name + pos ; *p; p++) if (*p == '\\') *p = '/';
        if (!stat( unix_name, &st ))
        {
            /* creation fails with STATUS_ACCESS_DENIED for the root of the drive */
            if (disposition == FILE_CREATE)
                return name_len ? STATUS_OBJECT_NAME_COLLISION : STATUS_ACCESS_DENIED;
            goto done;
        }
    }

    if (!name_len)  /* empty name -> drive root doesn't exist */
    {
        RtlFreeHeap( GetProcessHeap(), 0, unix_name );
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (check_case && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
    {
        RtlFreeHeap( GetProcessHeap(), 0, unix_name );
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* now do it component by component */

    while (name_len)
    {
        const WCHAR *end, *next;

        end = name;
        while (end < name + name_len && !IS_SEPARATOR(*end)) end++;
        next = end;
        while (next < name + name_len && IS_SEPARATOR(*next)) next++;
        name_len -= next - name;

        /* grow the buffer if needed */

        if (unix_len - pos < MAX_DIR_ENTRY_LEN + 2)
        {
            char *new_name;
            unix_len += 2 * MAX_DIR_ENTRY_LEN;
            if (!(new_name = RtlReAllocateHeap( GetProcessHeap(), 0, unix_name, unix_len )))
            {
                RtlFreeHeap( GetProcessHeap(), 0, unix_name );
                return STATUS_NO_MEMORY;
            }
            unix_name = new_name;
        }

        status = find_file_in_dir( unix_name, pos, name, end - name, check_case );

        /* if this is the last element, not finding it is not necessarily fatal */
        if (!name_len)
        {
            if (status == STATUS_OBJECT_PATH_NOT_FOUND)
            {
                status = STATUS_OBJECT_NAME_NOT_FOUND;
                if (disposition != FILE_OPEN && disposition != FILE_OVERWRITE)
                {
                    ret = ntdll_wcstoumbs( 0, name, end - name, unix_name + pos + 1,
                                           MAX_DIR_ENTRY_LEN, NULL, &used_default );
                    if (ret > 0 && !used_default)
                    {
                        unix_name[pos] = '/';
                        unix_name[pos + 1 + ret] = 0;
                        status = STATUS_NO_SUCH_FILE;
                        break;
                    }
                }
            }
            else if (status == STATUS_SUCCESS && disposition == FILE_CREATE)
            {
                status = STATUS_OBJECT_NAME_COLLISION;
            }
        }

        if (status != STATUS_SUCCESS)
        {
            /* couldn't find it at all, fail */
            WARN( "%s not found in %s\n", debugstr_w(name), unix_name );
            RtlFreeHeap( GetProcessHeap(), 0, unix_name );
            return status;
        }

        pos += strlen( unix_name + pos );
        name = next;
    }

    WARN( "%s -> %s required a case-insensitive search\n",
          debugstr_us(nameW), debugstr_a(unix_name) );

done:
    TRACE( "%s -> %s\n", debugstr_us(nameW), debugstr_a(unix_name) );
    unix_name_ret->Buffer = unix_name;
    unix_name_ret->Length = strlen(unix_name);
    unix_name_ret->MaximumLength = unix_len;
    return status;
}


/******************************************************************
 *		RtlDoesFileExists_U   (NTDLL.@)
 */
BOOLEAN WINAPI RtlDoesFileExists_U(LPCWSTR file_name)
{
    UNICODE_STRING nt_name;
    ANSI_STRING unix_name;
    BOOLEAN ret;

    if (!RtlDosPathNameToNtPathName_U( file_name, &nt_name, NULL, NULL )) return FALSE;
    ret = (wine_nt_to_unix_file_name( &nt_name, &unix_name, FILE_OPEN, FALSE ) == STATUS_SUCCESS);
    if (ret) RtlFreeAnsiString( &unix_name );
    RtlFreeUnicodeString( &nt_name );
    return ret;
}
