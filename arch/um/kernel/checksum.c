#include "asm/uaccess.h"
#include "linux/errno.h"
#include "linux/module.h"

unsigned int arch_csum_partial(const unsigned char *buff, int len, int sum);

unsigned int csum_partial(unsigned char *buff, int len, int sum)
{
	return arch_csum_partial(buff, len, sum);
}

EXPORT_SYMBOL(csum_partial);

unsigned int csum_partial_copy_to(const unsigned char *src, char __user *dst,
				int len, int sum, int *err_ptr)
{
	if(copy_to_user(dst, src, len)){
		*err_ptr = -EFAULT;
		return(-1);
	}

	return(arch_csum_partial(src, len, sum));
}

unsigned int csum_partial_copy_from(const unsigned char __user *src, char *dst,
				int len, int sum, int *err_ptr)
{
	if(copy_from_user(dst, src, len)){
		*err_ptr = -EFAULT;
		return(-1);
	}

	return arch_csum_partial(dst, len, sum);
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
