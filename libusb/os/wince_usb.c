/*
 * Windows CE backend for libusb 1.0
 * Copyright (C) 2011 RealVNC Ltd.
 * Large portions taken from Windows backend, which is
 * Copyright (C) 2009-2010 Pete Batard <pbatard@gmail.com>
 * With contributions from Michael Plante, Orin Eman et al.
 * Parts of this code adapted from libusb-win32-v1 by Stephan Meyer
 * Major code testing contribution by Xiaofan Chen
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "wince_usb.h"

#include <libusbi.h>

#include <stdint.h>
#include <errno.h>

// Forward declares
static int wince_clock_gettime(int clk_id, struct timespec *tp);
unsigned __stdcall wince_clock_gettime_threaded(void* param);

// Global variables
uint64_t hires_frequency, hires_ticks_to_ps;
int errno;
const uint64_t epoch_time = UINT64_C(116444736000000000);       // 1970.01.01 00:00:000 in MS Filetime
static int concurrent_usage = -1;
// Timer thread
// NB: index 0 is for monotonic and 1 is for the thread exit event
HANDLE timer_thread = NULL;
HANDLE timer_mutex = NULL;
struct timespec timer_tp;
volatile LONG request_count[2] = {0, 1};	// last one must be > 0
HANDLE timer_request[2] = { NULL, NULL };
HANDLE timer_response = NULL;
HANDLE driver_handle = INVALID_HANDLE_VALUE;

/*
 * Converts a windows error to human readable string
 * uses retval as errorcode, or, if 0, use GetLastError()
 */
#if defined(ENABLE_LOGGING)
static TCHAR* windows_error_str(uint32_t retval)
{
static TCHAR err_string[ERR_BUFFER_SIZE];

	DWORD size;
	size_t i;
	uint32_t error_code, format_error;

	error_code = retval?retval:GetLastError();
	
	safe_sprintf(err_string, ERR_BUFFER_SIZE, _T("[%d] "), error_code);
	
	size = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error_code,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), &err_string[safe_tcslen(err_string)],
		ERR_BUFFER_SIZE - (DWORD)safe_tcslen(err_string), NULL);
	if (size == 0) {
		format_error = GetLastError();
		if (format_error)
			safe_sprintf(err_string, ERR_BUFFER_SIZE,
				_T("Windows error code %u (FormatMessage error code %u)"), error_code, format_error);
		else
			safe_sprintf(err_string, ERR_BUFFER_SIZE, _T("Unknown error code %u"), error_code);
	} else {
		// Remove CR/LF terminators
		for (i=safe_tcslen(err_string)-1; ((err_string[i]==0x0A) || (err_string[i]==0x0D)); i--) {
			err_string[i] = 0;
		}
	}
	return err_string;
}
#endif

static struct wince_device_priv *_device_priv(struct libusb_device *dev)
{
        return (struct wince_device_priv *) dev->os_priv;
}

/* Hash table functions - modified From glibc 2.3.2:
   [Aho,Sethi,Ullman] Compilers: Principles, Techniques and Tools, 1986
   [Knuth]            The Art of Computer Programming, part 3 (6.4)  */
typedef struct htab_entry {
	unsigned long used;
	char* str;
} htab_entry;
htab_entry* htab_table = NULL;
usbi_mutex_t htab_write_mutex = NULL;
unsigned long htab_size, htab_filled;

/* For the used double hash method the table size has to be a prime. To
   correct the user given table size we need a prime test.  This trivial
   algorithm is adequate because the code is called only during init and
   the number is likely to be small  */
static int isprime(unsigned long number)
{
	// no even number will be passed
	unsigned int divider = 3;

	while((divider * divider < number) && (number % divider != 0))
		divider += 2;

	return (number % divider != 0);
}

/* Before using the hash table we must allocate memory for it.
   We allocate one element more as the found prime number says.
   This is done for more effective indexing as explained in the
   comment for the hash function.  */
static int htab_create(struct libusb_context *ctx, unsigned long nel)
{
	if (htab_table != NULL) {
		usbi_err(ctx, "hash table already allocated");
	}

	// Create a mutex
	usbi_mutex_init(&htab_write_mutex, NULL);

	// Change nel to the first prime number not smaller as nel.
	nel |= 1;
	while(!isprime(nel))
		nel += 2;

	htab_size = nel;
	usbi_dbg("using %d entries hash table", nel);
	htab_filled = 0;

	// allocate memory and zero out.
	htab_table = (htab_entry*)calloc(htab_size + 1, sizeof(htab_entry));
	if (htab_table == NULL) {
		usbi_err(ctx, "could not allocate space for hash table");
		return 0;
	}

	return 1;
}

/* After using the hash table it has to be destroyed.  */
static void htab_destroy(void)
{
	size_t i;
	if (htab_table == NULL) {
		return;
	}

	for (i=0; i<htab_size; i++) {
		if (htab_table[i].used) {
			safe_free(htab_table[i].str);
		}
	}
	usbi_mutex_destroy(&htab_write_mutex);
	safe_free(htab_table);
}

/* This is the search function. It uses double hashing with open addressing.
   We use an trick to speed up the lookup. The table is created with one
   more element available. This enables us to use the index zero special.
   This index will never be used because we store the first hash index in
   the field used where zero means not used. Every other value means used.
   The used field can be used as a first fast comparison for equality of
   the stored and the parameter value. This helps to prevent unnecessary
   expensive calls of strcmp.  */
static unsigned long htab_hash(char* str)
{
	unsigned long hval, hval2;
	unsigned long idx;
	unsigned long r = 5381;
	int c;
	char* sz = str;

	// Compute main hash value (algorithm suggested by Nokia)
	while ((c = *sz++))
		r = ((r << 5) + r) + c;
	if (r == 0)
		++r;

	// compute table hash: simply take the modulus
	hval = r % htab_size;
	if (hval == 0)
		++hval;

	// Try the first index
	idx = hval;

	if (htab_table[idx].used) {
		if ( (htab_table[idx].used == hval)
		  && (safe_strcmp(str, htab_table[idx].str) == 0) ) {
			// existing hash
			return idx;
		}
		usbi_dbg("hash collision ('%s' vs '%s')", str, htab_table[idx].str);

		// Second hash function, as suggested in [Knuth]
		hval2 = 1 + hval % (htab_size - 2);

		do {
			// Because size is prime this guarantees to step through all available indexes
			if (idx <= hval2) {
				idx = htab_size + idx - hval2;
			} else {
				idx -= hval2;
			}

			// If we visited all entries leave the loop unsuccessfully
			if (idx == hval) {
				break;
			}

			// If entry is found use it.
			if ( (htab_table[idx].used == hval)
			  && (safe_strcmp(str, htab_table[idx].str) == 0) ) {
				return idx;
			}
		}
		while (htab_table[idx].used);
	}

	// Not found => New entry

	// If the table is full return an error
	if (htab_filled >= htab_size) {
		usbi_err(NULL, "hash table is full (%d entries)", htab_size);
		return 0;
	}

	// Concurrent threads might be storing the same entry at the same time
	// (eg. "simultaneous" enums from different threads) => use a mutex
	usbi_mutex_lock(&htab_write_mutex);
	// Just free any previously allocated string (which should be the same as
	// new one). The possibility of concurrent threads storing a collision
	// string (same hash, different string) at the same time is extremely low
	safe_free(htab_table[idx].str);
	htab_table[idx].used = hval;
	htab_table[idx].str = malloc(safe_strlen(str)+1);
	if (htab_table[idx].str == NULL) {
		usbi_err(NULL, "could not duplicate string for hash table");
		usbi_mutex_unlock(&htab_write_mutex);
		return 0;
	}
	memcpy(htab_table[idx].str, str, safe_strlen(str)+1);
	++htab_filled;
	usbi_mutex_unlock(&htab_write_mutex);

	return idx;
}


static int init_dllimports()
{
	DLL_LOAD(ceusbkwrapper.dll, UkwOpenDriver, TRUE);
	DLL_LOAD(ceusbkwrapper.dll, UkwGetDeviceList, TRUE);
	DLL_LOAD(ceusbkwrapper.dll, UkwReleaseDeviceList, TRUE);
	DLL_LOAD(ceusbkwrapper.dll, UkwGetDeviceAddress, TRUE);
	DLL_LOAD(ceusbkwrapper.dll, UkwGetDeviceDescriptor, TRUE);
	DLL_LOAD(ceusbkwrapper.dll, UkwCloseDriver, TRUE);
	DLL_LOAD(ceusbkwrapper.dll, UkwCancelTransfer, TRUE);
	DLL_LOAD(ceusbkwrapper.dll, UkwIssueControlTransfer, TRUE);
	return LIBUSB_SUCCESS;
}

static int init_device(struct libusb_device *dev, UKW_DEVICE drv_dev,
					   unsigned char bus_addr, unsigned char dev_addr)
{
	struct wince_device_priv *priv = _device_priv(dev);
	int r;

	dev->bus_number = bus_addr;
	dev->device_address = dev_addr;
	priv->dev = drv_dev;

	r = UkwGetDeviceDescriptor(priv->dev, &(priv->desc));
	if (r < 0)
		goto out;
	// TODO: cache config descriptor too
out:
	return r;
}

// Internal API functions
static int wince_init(struct libusb_context *ctx)
{
	int i, r = LIBUSB_ERROR_OTHER;
	HANDLE semaphore;
	TCHAR sem_name[11+1+8]; // strlen(libusb_init)+'\0'+(32-bit hex PID)

	_stprintf(sem_name, _T("libusb_init%08X"), (unsigned int)GetCurrentProcessId()&0xFFFFFFFF);
	semaphore = CreateSemaphore(NULL, 1, 1, sem_name);
	if (semaphore == NULL) {
		usbi_err(ctx, "could not create semaphore: %s", windows_error_str(0));
		return LIBUSB_ERROR_NO_MEM;
	}

	// A successful wait brings our semaphore count to 0 (unsignaled)
	// => any concurent wait stalls until the semaphore's release
	if (WaitForSingleObject(semaphore, INFINITE) != WAIT_OBJECT_0) {
		usbi_err(ctx, "failure to access semaphore: %s", windows_error_str(0));
		CloseHandle(semaphore);
		return LIBUSB_ERROR_NO_MEM;
	}

	// NB: concurrent usage supposes that init calls are equally balanced with
	// exit calls. If init is called more than exit, we will not exit properly
	if ( ++concurrent_usage == 0 ) {	// First init?
		// Initialize pollable file descriptors
		init_polling();

		// Load DLL imports
		if (init_dllimports() != LIBUSB_SUCCESS) {
			usbi_err(ctx, "could not resolve DLL functions");
			r = LIBUSB_ERROR_NOT_SUPPORTED;
			goto init_exit;
		}

		// try to open a handle to the driver
		driver_handle = UkwOpenDriver();
		if (driver_handle == INVALID_HANDLE_VALUE) {
			usbi_err(ctx, "could not connect to driver");
			r = LIBUSB_ERROR_NOT_SUPPORTED;
			goto init_exit;
		}

		// Windows CE doesn't have a way of specifying thread affinity, so this code
		// just has  to hope QueryPerformanceCounter doesn't report different values when
		// running on different cores.
		r = LIBUSB_ERROR_NO_MEM;
		for (i = 0; i < 2; i++) {
			timer_request[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
			if (timer_request[i] == NULL) {
				usbi_err(ctx, "could not create timer request event %d - aborting", i);
				goto init_exit;
			}
		}
		timer_response = CreateSemaphore(NULL, 0, MAX_TIMER_SEMAPHORES, NULL);
		if (timer_response == NULL) {
			usbi_err(ctx, "could not create timer response semaphore - aborting");
			goto init_exit;
		}
		timer_mutex = CreateMutex(NULL, FALSE, NULL);
		if (timer_mutex == NULL) {
			usbi_err(ctx, "could not create timer mutex - aborting");
			goto init_exit;
		}
		timer_thread = CreateThread(NULL, 0, wince_clock_gettime_threaded, NULL, 0, NULL);
		if (timer_thread == NULL) {
			usbi_err(ctx, "Unable to create timer thread - aborting");
			goto init_exit;
		}

		// Create a hash table to store session ids. Second parameter is better if prime
		htab_create(ctx, HTAB_SIZE);
	}
	// At this stage, either we went through full init successfully, or didn't need to
	r = LIBUSB_SUCCESS;

init_exit: // Holds semaphore here.
	if (!concurrent_usage && r != LIBUSB_SUCCESS) { // First init failed?
		if (driver_handle != INVALID_HANDLE_VALUE) {
			UkwCloseDriver(driver_handle);
			driver_handle = INVALID_HANDLE_VALUE;
		}
		if (timer_thread) {
			SetEvent(timer_request[1]); // actually the signal to quit the thread.
			if (WAIT_OBJECT_0 != WaitForSingleObject(timer_thread, INFINITE)) {
				usbi_warn(ctx, "could not wait for timer thread to quit");
				TerminateThread(timer_thread, 1); // shouldn't happen, but we're destroying
												  // all objects it might have held anyway.
			}
			CloseHandle(timer_thread);
			timer_thread = NULL;
		}
		for (i = 0; i < 2; i++) {
			if (timer_request[i]) {
				CloseHandle(timer_request[i]);
				timer_request[i] = NULL;
			}
		}
		if (timer_response) {
			CloseHandle(timer_response);
			timer_response = NULL;
		}
		if (timer_mutex) {
			CloseHandle(timer_mutex);
			timer_mutex = NULL;
		}
		htab_destroy();
	}

	if (r != LIBUSB_SUCCESS)
		--concurrent_usage; // Not expected to call libusb_exit if we failed.

	ReleaseSemaphore(semaphore, 1, NULL);	// increase count back to 1
	CloseHandle(semaphore);
	return r;
}

static void wince_exit(void)
{
	int i;
	HANDLE semaphore;
	TCHAR sem_name[11+1+8]; // strlen(libusb_init)+'\0'+(32-bit hex PID)

	_stprintf(sem_name, _T("libusb_init%08X"), (unsigned int)GetCurrentProcessId()&0xFFFFFFFF);
	semaphore = CreateSemaphore(NULL, 1, 1, sem_name);
	if (semaphore == NULL) {
		return;
	}

	// A successful wait brings our semaphore count to 0 (unsignaled)
	// => any concurent wait stalls until the semaphore release
	if (WaitForSingleObject(semaphore, INFINITE) != WAIT_OBJECT_0) {
		CloseHandle(semaphore);
		return;
	}

	// Only works if exits and inits are balanced exactly
	if (--concurrent_usage < 0) {	// Last exit
		exit_polling();

		if (timer_thread) {
			SetEvent(timer_request[1]); // actually the signal to quit the thread.
			if (WAIT_OBJECT_0 != WaitForSingleObject(timer_thread, INFINITE)) {
				usbi_dbg("could not wait for timer thread to quit");
				TerminateThread(timer_thread, 1);
			}
			CloseHandle(timer_thread);
			timer_thread = NULL;
		}
		for (i = 0; i < 2; i++) {
			if (timer_request[i]) {
				CloseHandle(timer_request[i]);
				timer_request[i] = NULL;
			}
		}
		if (timer_response) {
			CloseHandle(timer_response);
			timer_response = NULL;
		}
		if (timer_mutex) {
			CloseHandle(timer_mutex);
			timer_mutex = NULL;
		}
		htab_destroy();

		if (driver_handle != INVALID_HANDLE_VALUE) {
			UkwCloseDriver(driver_handle);
			driver_handle = INVALID_HANDLE_VALUE;
		}
	}

	ReleaseSemaphore(semaphore, 1, NULL);	// increase count back to 1
	CloseHandle(semaphore);
}

static int wince_get_device_list(
	struct libusb_context *ctx,
	struct discovered_devs **discdevs)
{
	UKW_DEVICE devices[MAX_DEVICE_COUNT];
	struct discovered_devs * new_devices = *discdevs;
	DWORD count = 0, i;
	struct libusb_device *dev;
	unsigned char bus_addr, dev_addr;
	unsigned long session_id;
	BOOL success, need_unref = FALSE;
	int r = LIBUSB_SUCCESS;

	success = UkwGetDeviceList(driver_handle, devices, MAX_DEVICE_COUNT, &count);
	if (!success) {
		usbi_err(ctx, "could not get devices: %s", windows_error_str(0));
		return LIBUSB_ERROR_OTHER;
	}
	for(i = 0; i < count; ++i) {
		success = UkwGetDeviceAddress(devices[i], &bus_addr, &dev_addr, &session_id);
		if (!success) {
			usbi_err(ctx, "could not get device address for %d: %s", i, windows_error_str(0));
			r = LIBUSB_ERROR_OTHER;
			goto err_out;
		}
		dev = usbi_get_device_by_session_id(ctx, session_id);
		if (dev) {
			usbi_dbg("using existing device for %d/%d (session %ld)",
					bus_addr, dev_addr, session_id);
		} else {
			usbi_dbg("allocating new device for %d/%d (session %ld)",
					bus_addr, dev_addr, session_id);
			dev = usbi_alloc_device(ctx, session_id);
			if (!dev) {
				r = LIBUSB_ERROR_NO_MEM;
				goto err_out;
			}
			need_unref = TRUE;
			r = init_device(dev, devices[i], bus_addr, dev_addr);
			if (r < 0)
				goto err_out;
			r = usbi_sanitize_device(dev);
			if (r < 0)
				goto err_out;
		}
		new_devices = discovered_devs_append(new_devices, dev);
		if (!discdevs) {
			r = LIBUSB_ERROR_NO_MEM;
			goto err_out;
		}
		need_unref = FALSE;
	}
	*discdevs = new_devices;
	return r;
err_out:
	*discdevs = new_devices;
	if (need_unref)
		libusb_unref_device(dev);
	UkwReleaseDeviceList(driver_handle, devices, count);
	return r;
}

static int wince_open(struct libusb_device_handle *handle)
{
	// Nothing to do to open devices as a handle to it has
	// been retrieved by wince_get_device_list
	return LIBUSB_SUCCESS;
}

static void wince_close(struct libusb_device_handle *handle)
{
	// Nothing to do as wince_open does nothing.
}

static int wince_get_device_descriptor(
   struct libusb_device *device,
   unsigned char *buffer, int *host_endian)
{
	struct wince_device_priv *priv = _device_priv(device);

	*host_endian = 1;
	memcpy(buffer, &priv->desc, DEVICE_DESC_LENGTH);
	return LIBUSB_SUCCESS;
}

static int wince_get_active_config_descriptor(
	struct libusb_device *device,
	unsigned char *buffer, size_t len, int *host_endian)
{
	struct wince_device_priv *priv = _device_priv(device);
	DWORD actualSize = len;
	*host_endian = 1;
	if (!UkwGetConfigDescriptor(priv->dev, UKW_ACTIVE_CONFIGURATION, buffer, len, &actualSize)) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	return actualSize;
}

static int wince_get_config_descriptor(
	struct libusb_device *device,
	uint8_t config_index,
	unsigned char *buffer, size_t len, int *host_endian)
{
	struct wince_device_priv *priv = _device_priv(device);
	DWORD actualSize = len;
	*host_endian = 1;
	if (!UkwGetConfigDescriptor(priv->dev, config_index, buffer, len, &actualSize)) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	return actualSize;
}

static int wince_get_configuration(
   struct libusb_device_handle *handle,
   int *config)
{
	struct wince_device_priv *priv = _device_priv(handle->dev);
	UCHAR cv = 0;
	if (!UkwGetConfig(priv->dev, &cv)) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	(*config) = cv;
	return 0;
}

static int wince_set_configuration(
	struct libusb_device_handle *handle,
	int config)
{
	struct wince_device_priv *priv = _device_priv(handle->dev);
	// Setting configuration 0 places the device in Address state.
	// This should correspond to the "unconfigured state" required by
	// libusb when the specified configuration is -1.
	UCHAR cv = (config < 0) ? 0 : config;
	if (!UkwSetConfig(priv->dev, cv)) {
		switch (GetLastError())
		{
		case ERROR_NOT_SUPPORTED:
			return LIBUSB_ERROR_NOT_SUPPORTED;
		case ERROR_INVALID_PARAMETER:
		case ERROR_INVALID_HANDLE:
			return LIBUSB_ERROR_INVALID_PARAM;
		default:
			return LIBUSB_ERROR_NOT_FOUND;
		}
	}
	return 0;
}

static int wince_claim_interface(
	struct libusb_device_handle *handle,
	int interface_number)
{
	struct wince_device_priv *priv = _device_priv(handle->dev);
	if (!UkwClaimInterface(priv->dev, interface_number)) {
		return LIBUSB_ERROR_OTHER;
	}
	return LIBUSB_SUCCESS;
}

static int wince_release_interface(
	struct libusb_device_handle *handle,
	int interface_number)
{
	struct wince_device_priv *priv = _device_priv(handle->dev);
	if (!UkwSetInterfaceAlternateSetting(priv->dev, interface_number, 0)) {
		return LIBUSB_ERROR_IO;
	}
	if (!UkwReleaseInterface(priv->dev, interface_number)) {
		return LIBUSB_ERROR_OTHER;
	}
	return LIBUSB_SUCCESS;
}

static int wince_set_interface_altsetting(
	struct libusb_device_handle *handle,
	int interface_number, int altsetting)
{
	struct wince_device_priv *priv = _device_priv(handle->dev);
	if (!UkwSetInterfaceAlternateSetting(priv->dev, interface_number, altsetting)) {
		return LIBUSB_ERROR_IO;
	}
	return LIBUSB_SUCCESS;
}

static int wince_clear_halt(
	struct libusb_device_handle *handle,
	unsigned char endpoint)
{
	struct wince_device_priv *priv = _device_priv(handle->dev);
	if (!UkwClearHalt(priv->dev, endpoint)) {
		return LIBUSB_ERROR_IO;
	}
	return LIBUSB_SUCCESS;
}

static int wince_reset_device(
	struct libusb_device_handle *handle)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int wince_kernel_driver_active(
	struct libusb_device_handle *handle,
	int interface_number)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int wince_detach_kernel_driver(
	struct libusb_device_handle *handle,
	int interface_number)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int wince_attach_kernel_driver(
	struct libusb_device_handle *handle,
	int interface_number)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static void wince_destroy_device(
	struct libusb_device *dev)
{

}

static int wince_submit_transfer(
	struct usbi_transfer *itransfer)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int wince_cancel_transfer(
	struct usbi_transfer *itransfer)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int wince_clear_transfer_priv(
	struct usbi_transfer *itransfer)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int wince_handle_events(
	struct libusb_context *ctx,
	struct pollfd *fds, POLL_NFDS_TYPE nfds, int num_ready)
{
	return LIBUSB_ERROR_NOT_SUPPORTED;
}

/*
 * Monotonic and real time functions
 */
unsigned __stdcall wince_clock_gettime_threaded(void* param)
{
	LARGE_INTEGER hires_counter, li_frequency;
	LONG nb_responses;
	int timer_index;

	// Init - find out if we have access to a monotonic (hires) timer
	if (!QueryPerformanceFrequency(&li_frequency)) {
		usbi_dbg("no hires timer available on this platform");
		hires_frequency = 0;
		hires_ticks_to_ps = UINT64_C(0);
	} else {
		hires_frequency = li_frequency.QuadPart;
		// The hires frequency can go as high as 4 GHz, so we'll use a conversion
		// to picoseconds to compute the tv_nsecs part in clock_gettime
		hires_ticks_to_ps = UINT64_C(1000000000000) / hires_frequency;
		usbi_dbg("hires timer available (Frequency: %"PRIu64" Hz)", hires_frequency);
	}

	// Main loop - wait for requests
	while (1) {
		timer_index = WaitForMultipleObjects(2, timer_request, FALSE, INFINITE) - WAIT_OBJECT_0;
		if ( (timer_index != 0) && (timer_index != 1) ) {
			usbi_dbg("failure to wait on requests: %s", windows_error_str(0));
			continue;
		}
		if (request_count[timer_index] == 0) {
			// Request already handled
			ResetEvent(timer_request[timer_index]);
			// There's still a possiblity that a thread sends a request between the
			// time we test request_count[] == 0 and we reset the event, in which case
			// the request would be ignored. The simple solution to that is to test
			// request_count again and process requests if non zero.
			if (request_count[timer_index] == 0)
				continue;
		}
		switch (timer_index) {
		case 0:
			WaitForSingleObject(timer_mutex, INFINITE);
			// Requests to this thread are for hires always
			if (QueryPerformanceCounter(&hires_counter) != 0) {
				timer_tp.tv_sec = (long)(hires_counter.QuadPart / hires_frequency);
				timer_tp.tv_nsec = (long)(((hires_counter.QuadPart % hires_frequency)/1000) * hires_ticks_to_ps);
			} else {
				// Fallback to real-time if we can't get monotonic value
				// Note that real-time clock does not wait on the mutex or this thread.
				wince_clock_gettime(USBI_CLOCK_REALTIME, &timer_tp);
			}
			ReleaseMutex(timer_mutex);

			nb_responses = InterlockedExchange((LONG*)&request_count[0], 0);
			if ( (nb_responses)
			  && (ReleaseSemaphore(timer_response, nb_responses, NULL) == 0) ) {
				usbi_dbg("unable to release timer semaphore %d: %s", windows_error_str(0));
			}
			continue;
		case 1: // time to quit
			usbi_dbg("timer thread quitting");
			return 0;
		}
	}
	usbi_dbg("ERROR: broken timer thread");
	return 1;
}

static int wince_clock_gettime(int clk_id, struct timespec *tp)
{
	FILETIME filetime;
	ULARGE_INTEGER rtime;
	DWORD r;
	SYSTEMTIME st;
	switch(clk_id) {
	case USBI_CLOCK_MONOTONIC:
		if (hires_frequency != 0) {
			while (1) {
				InterlockedIncrement((LONG*)&request_count[0]);
				SetEvent(timer_request[0]);
				r = WaitForSingleObject(timer_response, TIMER_REQUEST_RETRY_MS);
				switch(r) {
				case WAIT_OBJECT_0:
					WaitForSingleObject(timer_mutex, INFINITE);
					*tp = timer_tp;
					ReleaseMutex(timer_mutex);
					return LIBUSB_SUCCESS;
				case WAIT_TIMEOUT:
					usbi_dbg("could not obtain a timer value within reasonable timeframe - too much load?");
					break; // Retry until successful
				default:
					usbi_dbg("WaitForSingleObject failed: %s", windows_error_str(0));
					return LIBUSB_ERROR_OTHER;
				}
			}
		}
		// Fall through and return real-time if monotonic was not detected @ timer init
	case USBI_CLOCK_REALTIME:
		// We follow http://msdn.microsoft.com/en-us/library/ms724928%28VS.85%29.aspx
		// with a predef epoch_time to have an epoch that starts at 1970.01.01 00:00
		// Note however that our resolution is bounded by the Windows system time
		// functions and is at best of the order of 1 ms (or, usually, worse)
		GetSystemTime(&st);
		SystemTimeToFileTime(&st, &filetime);
		rtime.LowPart = filetime.dwLowDateTime;
		rtime.HighPart = filetime.dwHighDateTime;
		rtime.QuadPart -= epoch_time;
		tp->tv_sec = (long)(rtime.QuadPart / 10000000);
		tp->tv_nsec = (long)((rtime.QuadPart % 10000000)*100);
		return LIBUSB_SUCCESS;
	default:
		return LIBUSB_ERROR_INVALID_PARAM;
	}
}

const struct usbi_os_backend wince_backend = {
        "Windows CE",
        wince_init,
        wince_exit,

        wince_get_device_list,
        wince_open,
        wince_close,

        wince_get_device_descriptor,
        wince_get_active_config_descriptor,
        wince_get_config_descriptor,

        wince_get_configuration,
        wince_set_configuration,
        wince_claim_interface,
        wince_release_interface,

        wince_set_interface_altsetting,
        wince_clear_halt,
        wince_reset_device,

        wince_kernel_driver_active,
        wince_detach_kernel_driver,
        wince_attach_kernel_driver,

        wince_destroy_device,

        wince_submit_transfer,
        wince_cancel_transfer,
        wince_clear_transfer_priv,

        wince_handle_events,

        wince_clock_gettime,
#if defined(USBI_TIMERFD_AVAILABLE)
        NULL,
#endif
        sizeof(struct wince_device_priv),
        sizeof(struct wince_device_handle_priv),
        sizeof(struct wince_transfer_priv),
        0,
};