#include "libusbi.h"
#include "threads_windows.h"

#include "windows_common.h"
#include "windows_hotplug.h"

#include <stdio.h>
#include <dbt.h>
#include <usbiodef.h>

#define USB_REFRESH_TIMER_ID 1

#define for_each_pending_change(c) \
	list_for_each_entry(c, &windows_pending_changes, list, struct windows_pending_change)

#define for_each_pending_change_safe(c, n) \
	list_for_each_entry_safe(c, n, &windows_pending_changes, list, struct windows_pending_change)

/* The Windows Hotplug system is a three steps process.
 * 1. We create a monitor on GUID_DEVINTERFACE_USB_DEVICE via a hidden window.
 * 2. Upon notification of an event, we run the current windows backend to get the list of devices.
 *    This updates the hotplug status of each device to one of three values {UNCHANGED, ARRIVED, LEFT}.
 * 3. According to the value, we generate events to libusb client via hotplug callbacks. */

struct windows_pending_change {
	struct list_head list;
	WPARAM           event;
	char *           device_name;
};

static HWND windows_event_hwnd;
static HANDLE windows_event_thread_handle;
static struct list_head windows_pending_changes;
static DWORD WINAPI windows_event_thread_main(LPVOID lpParam);
static LRESULT CALLBACK windows_proc_callback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

static void windows_clear_pending_changes(void);
static void windows_pending_change_free(struct windows_pending_change *change);

#define log_error(operation) do { \
	usbi_err(NULL, "%s failed with error: %s", operation, windows_error_str(0)); \
} while (0)

int windows_start_event_monitor(void)
{
	list_init(&windows_pending_changes);

	windows_event_thread_handle = CreateThread(
		NULL, // Default security descriptor
		0, // Default stack size
		windows_event_thread_main,
		NULL, // No parameters to pass to the thread
		0, // Start immediately
		NULL // No need to keep track of thread ID
	);

	if (windows_event_thread_handle == NULL)
	{
		log_error("CreateThread");
		return LIBUSB_ERROR_OTHER;
	}

	return LIBUSB_SUCCESS;
}

int windows_stop_event_monitor(void)
{
	if (windows_event_hwnd == NULL)
	{
		return LIBUSB_SUCCESS;
	}

	if (!SUCCEEDED(SendMessage(windows_event_hwnd, WM_CLOSE, 0, 0)))
	{
		log_error("SendMessage");
		return LIBUSB_ERROR_OTHER;
	}

	if (WaitForSingleObject(windows_event_thread_handle, INFINITE) != WAIT_OBJECT_0)
	{
		log_error("WaitForSingleObject");
		return LIBUSB_ERROR_OTHER;
	}

	if (!CloseHandle(windows_event_thread_handle))
	{
		log_error("CloseHandle");
		return LIBUSB_ERROR_OTHER;
	}

	windows_clear_pending_changes();

	return LIBUSB_SUCCESS;
}

/* We leverage the backend's get_device_list method to discover new/gone devices.
 * 1. Mark all devices as if they LEFT
 * 2. Run get_device_list which will set devices still connected to UNCHANGED and new devices to ARRIVED.
 * Since the marker is not modified for devices which had disconnected and are thus no longer enumerated,
 * all devices have appropriate connection status and the end of the process */
static int windows_get_device_list(struct libusb_context *ctx)
{
	// note: context device list is protected by active_contexts_lock
	return ((struct windows_context_priv *)usbi_get_context_priv(ctx))->backend->get_device_list(ctx);
}

void windows_initial_scan_devices(struct libusb_context *ctx)
{
	usbi_mutex_static_lock(&active_contexts_lock);

	const int ret =  windows_get_device_list(ctx);
	if (ret != LIBUSB_SUCCESS)
	{
		usbi_err(ctx, "hotplug failed to retrieve initial list with error: %s", libusb_error_name(ret));
	}
	usbi_mutex_static_unlock(&active_contexts_lock);
}

static void windows_refresh_device_list(struct libusb_context *ctx)
{
	struct windows_pending_change *change;

	for_each_pending_change(change) {
		struct libusb_device *dev, *next_dev;
		struct winusb_device_priv *priv;

		if (change->event != DBT_DEVICEREMOVECOMPLETE)
			continue;

		for_each_device_safe(ctx, dev, next_dev) {
			priv = usbi_get_device_priv(dev);

			if (_stricmp(priv->path, change->device_name) != 0)
				continue;

			if (priv->initialized)
				usbi_disconnect_device(dev);
			else
				usbi_detach_device(dev);
		}
	}

	const int ret = windows_get_device_list(ctx);
	if (ret != LIBUSB_SUCCESS)
	{
		usbi_err(ctx, "hotplug failed to retrieve current list with error: %s", libusb_error_name(ret));
		return;
	}

	for_each_pending_change(change) {
		struct libusb_device *dev, *next_dev;
		struct winusb_device_priv *priv;

		if (change->event != DBT_DEVICEARRIVAL)
			continue;

		for_each_device_safe(ctx, dev, next_dev) {
			priv = usbi_get_device_priv(dev);

			if (_stricmp(priv->path, change->device_name) != 0)
				continue;

			usbi_hotplug_notification(ctx, dev, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
		}
	}
}

static void windows_refresh_device_list_for_all_ctx(void)
{
	usbi_mutex_static_lock(&active_contexts_lock);

	struct libusb_context *ctx;
	for_each_context(ctx)
	{
		windows_refresh_device_list(ctx);
	}

	usbi_mutex_static_unlock(&active_contexts_lock);

	windows_clear_pending_changes();
}

#define WND_CLASS_NAME TEXT("libusb-1.0-windows-hotplug")

static bool init_wnd_class(void)
{
	WNDCLASS wndClass = { 0 };
	wndClass.lpfnWndProc = windows_proc_callback;
	wndClass.hInstance = GetModuleHandle(NULL);
	wndClass.lpszClassName = WND_CLASS_NAME;

	if (!RegisterClass(&wndClass))
	{
		log_error("event thread: RegisterClass");
		return false;
	}

	return true;
}

static DWORD WINAPI windows_event_thread_main(LPVOID lpParam)
{
	UNUSED(lpParam);

	usbi_dbg(NULL, "windows event thread entering");

	if (!init_wnd_class())
	{
		return (DWORD)-1;
	}

	windows_event_hwnd = CreateWindow(
		WND_CLASS_NAME,
		TEXT(""),
		0,
		0, 0, 0, 0,
		NULL, NULL,
		GetModuleHandle(NULL),
		NULL);

	if (windows_event_hwnd == NULL)
	{
		log_error("event thread: CreateWindow");
		return (DWORD)-1;
	}

	MSG msg;
	BOOL ret_val;

	while ((ret_val = GetMessage(&msg, windows_event_hwnd, 0, 0)) != 0)
	{
		if (ret_val == -1)
		{
			log_error("event thread: GetMessage");
			break;
		}

		if (!SUCCEEDED(TranslateMessage(&msg)))
		{
			log_error("event thread: TranslateMessage");
		}

		if (!SUCCEEDED(DispatchMessage(&msg)))
		{
			log_error("event thread: DispatchMessage");
		}
	}

	usbi_dbg(NULL, "windows event thread exiting");

	return 0;
}

static bool register_device_interface_to_window_handle(
	IN GUID interface_class_guid,
	IN HWND hwnd,
	OUT HDEVNOTIFY* device_notify_handle)
{
	DEV_BROADCAST_DEVICEINTERFACE notificationFilter = { 0 };
	notificationFilter.dbcc_size = sizeof(notificationFilter);
	notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	notificationFilter.dbcc_classguid = interface_class_guid;

	*device_notify_handle = RegisterDeviceNotification(
		hwnd,
		&notificationFilter,
		DEVICE_NOTIFY_WINDOW_HANDLE
	);

	if (*device_notify_handle == NULL)
	{
		log_error("register_device_interface_to_window_handle");
		return false;
	}

	return true;
}

static LRESULT CALLBACK windows_proc_callback(
	HWND hwnd,
	UINT message,
	WPARAM wParam,
	LPARAM lParam)
{
	UNUSED(lParam);

	static HDEVNOTIFY device_notify_handle;

	switch (message)
	{
	case WM_CREATE:
		if (!register_device_interface_to_window_handle(
			GUID_DEVINTERFACE_USB_DEVICE,
			hwnd,
			&device_notify_handle))
		{
			return -1;
		}
		return 0;

	case WM_DEVICECHANGE:
		switch (wParam)
		{
		case DBT_DEVICEARRIVAL:
		case DBT_DEVICEREMOVECOMPLETE:
			if (((PDEV_BROADCAST_HDR)lParam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
			{
#ifdef UNICODE
				char* device_name = NULL;
				const WCHAR* w_dbcc_name = ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name;

				const int len = WideCharToMultiByte(CP_UTF8, 0, w_dbcc_name, -1, NULL, 0, NULL, NULL);
			    if (len == 0) 
				{
			        log_error("Conversion length calculation failed for ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name conversion from wchar to char");
			    }
				else
				{
					device_name = (char *)malloc(len);
				    if (device_name == NULL) 
					{
				        log_error("Memory allocation failed for ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name conversion from wchar to char");
				    }
					else
					{
					    const int result = WideCharToMultiByte(CP_UTF8, 0, w_dbcc_name, -1, device_name, len, NULL, NULL);
					    if (result == 0) 
						{
					        log_error("Conversion failed for ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name conversion from wchar to char");
					        free(device_name);
							device_name = NULL;						        
					    }	
					}
    			}
#else
				const char* device_name = ((PDEV_BROADCAST_DEVICEINTERFACE)lParam)->dbcc_name;
#endif

				struct windows_pending_change *change;

				bool still_relevant = true;
				if (wParam == DBT_DEVICEREMOVECOMPLETE) {
					for_each_pending_change(change) {
						if (change->event == DBT_DEVICEARRIVAL && _stricmp(change->device_name, device_name) == 0) {
							windows_pending_change_free(change);
							still_relevant = false;
							break;
						}
					}
				}

				if (still_relevant) {
					bool is_first_pending_change = list_empty(&windows_pending_changes);

					change = calloc(1, sizeof(*change));
					change->event = wParam;
					change->device_name = _strdup(device_name);
					list_add_tail(&change->list, &windows_pending_changes);
				}

#ifdef UNICODE
				free(device_name);
#endif
				return TRUE;
			}
			break;
		case DBT_DEVNODES_CHANGED:
			if (!list_empty(&windows_pending_changes)) {
				UINT num_arrivals = 0;
				UINT num_removals = 0;
				struct windows_pending_change *change;
				for_each_pending_change(change) {
						if (change->event == DBT_DEVICEARRIVAL)
							num_arrivals++;
						else
							num_removals++;
				}
				bool change_potentially_related_to_removal = num_arrivals != 0 && num_removals != 0;
				UINT delay = change_potentially_related_to_removal ? 500 : 50;
				SetTimer(hwnd, USB_REFRESH_TIMER_ID, delay, NULL);
			}
			break;
		}
		return BROADCAST_QUERY_DENY;

	case WM_TIMER:
		KillTimer(hwnd, USB_REFRESH_TIMER_ID);
		windows_refresh_device_list_for_all_ctx();
		return 0;

	case WM_CLOSE:
		if (!UnregisterDeviceNotification(device_notify_handle))
		{
			log_error("UnregisterDeviceNotification");
		}
		if (!DestroyWindow(hwnd))
		{
			log_error("DestroyWindow");
		}
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	default:
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
}

static void windows_clear_pending_changes(void)
{
	struct windows_pending_change *change, *next;
	for_each_pending_change_safe(change, next) {
		windows_pending_change_free(change);
	}
}

static void windows_pending_change_free(struct windows_pending_change *change)
{
	list_del(&change->list);
	free(change->device_name);
	free(change);
}
