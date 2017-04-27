/*
 * gprscdma.c
 *
 *  Created on: 2015-8-15
 *      Author: Johnnyzhang
 */

#include "gprscdma.h"
#include "main.h"
#include "common.h"
#include "serial.h"
#include "menu.h"
#include "gpio.h"
#include "atcmd.h"
#include "cm180.h"
#include "sim900a.h"
#include "m590e.h"

#define MODEM_PCL_STACK_IN_OS		0
#define MODEM_PCL_STACK_IN_MODULE	1

#define MODEM_INVALID_SIGNAL_INTENSITY 99

#define MODEM_CONNECT_SOCKET_TYPE_TCP 1
#define MODEM_CONNECT_SOCKET_TYPE_UDP 2
#define MODEM_CONNECT_SOCKET_TYPE_UNKNOWN 3

#define M590E_KEEP_TCP_CONNECT_INTERVAL_TIME (5 * 10)

typedef struct {
	int (*ppp_connect)(const char *device_name, const char *lock_name,
			const char *baudstr);
	int (*tcp_connect)(int fd, const char *addr, int port, int timeout);
	int (*udp_connect)(int fd, const char *addr, int port);
	int (*send)(int fd, const BYTE *buf, int len, int *errcode); /// n -> send
	int (*receive)(int fd, BYTE *buf, int maxlen, int timeout, int *errcode);
	int (*shutdown)(int fd);
	BOOL (*getip)(int fd, char *ipstr);
} PROTOCOL_STACK;

typedef struct {
	e_remote_module_model model;
	const char *describe;

	int remote_fd;
	int socket_type;
	void *presource;
	e_remote_module_status (*init)(int fd);
	PROTOCOL_STACK pcl_in_module;
	PROTOCOL_STACK pcl_in_os;
	BYTE net_type;
	BYTE signal_intensity;
	BYTE protocol_stack_type;
} REMOTE_MODULE_ATTR;

static REMOTE_MODULE_ATTR *pcur_module_attr = NULL;

static int read_lock_file(const char *name) {
	int fd, len, pid = -1;
	char apid[20];

	if ((fd = open(name, O_RDONLY)) == -1)
		return -1;
	len = safe_read(fd, apid, sizeof(apid) - 1);
	close(fd);
	if (len <= 0)
		return -1;
	apid[len] = 0;
	sscanf(apid, "%d", &pid); /// 
	return pid;
}

static int write_lock_file(const char *lockfile) {
	int fd, pid, lock_pid, len;
	char buf[PATH_MAX + 1], apid[16];

	snprintf(buf, sizeof(buf), "/var/lock/%s", "TM.XXXXXX"); /// 
	if ((fd = mkstemp(buf)) < 0) /// system call
		return 0;
	chmod(buf, 0644); /// system call
	pid = getpid(); /// now pid
	len = snprintf(apid, sizeof(apid), "%10d\n", pid);
	safe_write(fd, apid, len);
	close(fd);
	while (link(buf, lockfile) == -1) {
		if (errno == EEXIST) {
			lock_pid = read_lock_file(lockfile);
			if (lock_pid == pid)
				break;
			if (lock_pid < 0)
				continue;
			if (lock_pid == 0 || kill(lock_pid, 0) == -1) {
				unlink(lockfile);
				continue;
			}
		}
		unlink(buf);
		return 0;
	}
	unlink(buf);
	return 1;
}

static void modem_rtscts(int fd, int on) {
	struct termios tios;

	tcgetattr(fd, &tios); /// get
	if (on) {
		tios.c_cflag &= ~CLOCAL; /// 位反
		tios.c_cflag |= CRTSCTS; /// 使用RTS/CTS流控制
	} else {
		tios.c_cflag |= CLOCAL;
		tios.c_cflag &= ~CRTSCTS;
	}
	tcsetattr(fd, TCSANOW, &tios); /// set
}

int open_modem_device(const char *device_name, const char *lock_name,
		int baudrate) {
	int fd, lock_pid;

	/* added by wd */
	if ((pcur_module_attr != NULL) && (pcur_module_attr->remote_fd != -1)) { /// correspond to the close modem device
		return pcur_module_attr->remote_fd;
	}

	if (!write_lock_file(lock_name)) { // cannot write lock file
		lock_pid = read_lock_file(lock_name);
		PRINTF("%s is locked by %d\n", device_name, lock_pid);
		//lcd_update_head_enable(1);
		return -1;
	}
	if ((fd = open_serial(device_name, baudrate, 8, 0)) < 0) { /// not lock file and can write
		remove(lock_name);  /// open serial failed
		PRINTF("Open modem device FAIL\n");
		//lcd_update_head_enable(1);
		return -1;
	}

	PRINTF("Open modem device success fd: %d\n", fd);
	modem_rtscts(fd, 0); /// 设置
	return fd;
}

void close_modem_device(const char *lock_name) {
	int fd;

	if (pcur_module_attr == NULL)
		return; /// when test, return here

	fd = pcur_module_attr->remote_fd;
	if (fd >= 0) {
		close_serial(fd);
		pcur_module_attr->remote_fd = -1;
	}
	if (lock_name) {
		remove(lock_name); /// remove
	}
	PRINTF("Close modem device fd: %d\n", fd);
}

static e_remote_module_status check_network_status(int fd,
		REMOTE_MODULE_ATTR *pattr)
{
	int local_register_val = 1, remote_register_val = 5, val, i;
	BYTE modem_net_type;
	char resp[1024], *ptr;
	e_remote_module_status e_ret = e_modem_st_unknown_abort;

	if (NULL == pattr)
		return e_modem_st_unknown_abort;
	modem_net_type = pattr->net_type;
	lcd_update_comm_info(4);
	if (at_cmd(fd, "AT+CSQ\r", resp, sizeof(resp), 1000, 500)) {  /// signal
		if ((ptr = strstr(resp, "+CSQ:")) != NULL) {
			ptr += 5;
			sscanf(ptr, "%d", &val);
			pattr->signal_intensity = val;
			//lcd_update_head_enable(1);
			e_ret = e_modem_st_signal_intensity_abort;
			if (pattr->signal_intensity != 99 && pattr->signal_intensity > 11)
				e_ret = e_modem_st_register_network_fail;
			for (i = 0; !g_terminated && i < 3; i++) {
				if (at_cmd(fd, "AT+CREG?\r", resp, sizeof(resp), 1000, 500)) {
					/* response format +CREG:<n>,<stat> */
					if ((ptr = strstr(resp, "+CREG:")) != NULL
							&& (ptr = strstr(resp, ",")) != NULL) {
						ptr += 1; /* skip "<n>," */
						sscanf(ptr, "%d", &val);
						PRINTF("modem +CREG status: %d\n", val);
						if (val == local_register_val
								|| val == remote_register_val) {
							e_ret = e_modem_st_normal;
							break;
						} else {
							e_ret = e_modem_st_register_network_fail;
						}
					}
				}
				msleep(2000);
			}
		} else
			return e_modem_st_atcmd_abort;
	} else {
		lcd_update_comm_info(1);
		return e_modem_st_deivce_abort;
	}
	lcd_update_comm_info(6);
	return e_ret;
}

static REMOTE_MODULE_ATTR module_attr[] = {
		/// CDMA NEO CM180
		{ .model = e_cdma_model_neo_cm180, .describe = "NEO CM180",
			.init = cm180_init,
			.remote_fd = -1,
			.socket_type =MODEM_CONNECT_SOCKET_TYPE_UNKNOWN,
			.presource =&g_cm180_resource,
			.pcl_in_module = {
					.ppp_connect = cm180_ppp_connect,
					.tcp_connect = cm180_tcp_connect,
					.udp_connect = cm180_udp_connect,
					.send = cm180_send,
					.receive = cm180_receive,
					.shutdown = cm180_shutdown,
					.getip = cm180_getip, }, /// protocol stack of module
				.pcl_in_os = { .ppp_connect = NULL, .tcp_connect = NULL,
						.udp_connect = NULL, .send = NULL, .receive = NULL,
						.shutdown = NULL, }, /// protocol stack operating system
				.net_type = MODEM_NET_TYPE_CDMA, .signal_intensity =
						MODEM_INVALID_SIGNAL_INTENSITY, .protocol_stack_type =
						MODEM_PCL_STACK_IN_MODULE, },
		{ /// GPRS /// not use this
				.model = e_gprs_model_sim900,
				.describe = "SIM900A",
				.init = sim900a_init,
				.remote_fd = -1,
				.socket_type = MODEM_CONNECT_SOCKET_TYPE_UNKNOWN,
				.presource = NULL,
				.pcl_in_module = {
						.ppp_connect = sim900a_ppp_connect,
						.tcp_connect = sim900a_tcp_connect,
						.udp_connect = sim900a_udp_connect,
						.send = sim900a_send,
						.receive = sim900a_receive,
						.shutdown = sim900a_shutdown,
						.getip = sim900a_getip,
				},
				.pcl_in_os = {
						.ppp_connect = NULL,
						.tcp_connect = NULL,
						.udp_connect = NULL,
						.send = NULL,
						.receive = NULL,
						.shutdown = NULL,
				},
				.net_type = MODEM_NET_TYPE_GPRS,
				.signal_intensity = MODEM_INVALID_SIGNAL_INTENSITY,
				.protocol_stack_type = MODEM_PCL_STACK_IN_MODULE, },
		{ /// m590e r2
				.model = e_gprs_model_neo_m590e_r2,
				.describe = "NEO M590E R2",
				.init = m590e_init,
				.remote_fd = -1,
				.socket_type = MODEM_CONNECT_SOCKET_TYPE_UNKNOWN,
				.presource = &g_m590e_resource,
				.pcl_in_module = {
						.ppp_connect = m590e_ppp_connect,
						.tcp_connect = m590e_tcp_connect,
						.udp_connect = m590e_udp_connect,
						.send = m590e_send,
						.receive = m590e_receive, /// receive get value
						.shutdown = m590e_shutdown,
						.getip = m590e_getip, },
				.pcl_in_os = {
						.ppp_connect = NULL,
						.tcp_connect = NULL,
						.udp_connect = NULL,
						.send = NULL,
						.receive = NULL,
						.shutdown = NULL, },
				.net_type = MODEM_NET_TYPE_GPRS,
				.signal_intensity = MODEM_INVALID_SIGNAL_INTENSITY,
				.protocol_stack_type = MODEM_PCL_STACK_IN_MODULE, },
};

static REMOTE_MODULE_ATTR *get_modem_interface(e_remote_module_model model) /// standard pattern
{
	int i;

	for (i = 0; i < (sizeof(module_attr) / sizeof(module_attr[0])); i++) {
		if (model == module_attr[i].model) {
			return &module_attr[i];
		}
	}
	return NULL;
}

static REMOTE_MODULE_ATTR *parse_model(const char *str) {
	e_remote_module_model model;

	if (strstr(str, "CM180") != NULL) {
		model = e_cdma_model_neo_cm180;
	} else if (strstr(str, "M590 R2") != NULL) {
		model = e_gprs_model_neo_m590e_r2;
	} else if (strstr(str, "SIM900") != NULL) {
		model = e_gprs_model_sim900;
	} else {
		model = e_model_unknown;
	}
	return get_modem_interface(model);
}

static int try_sync_modem_baud(int fd) {
	char resp[1024];
	int i, ret = 0;

	memset(resp, 0, sizeof(resp));
	for (i = 0; i < 5 && !g_terminated; i++) {
		at_cmd(fd, "AT\r", resp, sizeof(resp), 1000, 500); // add timeout
		if (strstr(resp, "OK") != NULL) {
			ret = 1;
			break;
		}
		msleep(500);
	}
	return ret; /// ret=0,failed
}

///void modem_power_test(int fd);

static int test_modem_cmd(const char *device_name, const char *lock_name) {
	int fd, ret = 0, i;
	char resp[16];

	if ((fd = open_modem_device(device_name, lock_name, MODEM_DEFAULT_BAUD))
			< 0)
		return 0;
	for (i = 0; !g_terminated && i < 3; i++) { /// test 3 time
		at_cmd(fd, "AT\r", resp, sizeof(resp), 1000, 500);
		if (strstr(resp, "OK") != NULL) {
			ret = 1;
			break;
		}
		///wait_delay(1000);
		wait_delay(500); /// add by wd
	}
	///close_modem_device (lock_name); /// close fd and remove the lockname /// cannot modify upper data structure easily  /// lower function have no priviledge to modify upper data structure
	close_serial(fd);
	PRINTF("(%s): %s, device name: %s\n", __FUNCTION__,
			(ret > 0) ? "OK" : "FAIL", device_name);

	return ret;
}

static int modem_st_to_ret(e_remote_module_status status) /// this function's usage
{
	switch (status) {
	case e_modem_st_normal:
		PRINTF("Modem status: Normal\n");
		return 3;
	case e_modem_st_deivce_abort:
		PRINTF("Modem status: Device abort\n"); /// [2016-12-22 10:27:05.322] th_upgprscdma Modem status: Device abort
		return 0;
	case e_modem_st_atcmd_abort:
		PRINTF("Modem status: AT command abort\n");
		return 0;
	case e_modem_st_sim_card_abort:
		PRINTF("Modem status: SIM card abort\n");
		return 1;
	case e_modem_st_signal_intensity_abort:
		PRINTF("Modem status: Signal intensity abort\n");
		return 2;
	case e_modem_st_register_network_fail:
		PRINTF("Modem status: Register network fail\n");
		return 2;
	case e_modem_st_unknown_abort:
		PRINTF("Modem status: Unknown abort\n");
		return 0;
	default:
		PRINTF("Modem status: Invalid\n");
		return 0;
	}
}

static int do_modem_check(const char *device_name, const char *lock_name,
		int baudrate) /// check communicate with modem-hardware via serial-port
{
	int ret = 0, fd;
	char resp[1024];
	e_remote_module_status module_st;
	///int module_st;

	if (g_terminated)
		return 0;

	if ((fd = open_modem_device(device_name, lock_name, baudrate)) < 0) /// if return -1, file is locked
		return 0;

	if (!try_sync_modem_baud(fd)) {
		close_modem_device(lock_name);
		//lcd_update_head_enable(1);
		return 0;
	}

	if (!at_cmd(fd, "AT$MYGMR\r", resp, sizeof(resp), 1000, 500)) {
		close_modem_device(lock_name);
		//lcd_update_head_enable(3);
		return 0;
	} else {
		pcur_module_attr = parse_model(resp); /// get module information
	}

	if (NULL == pcur_module_attr) {
		ret = 0;
		close_serial(fd); /// add by wd, not necessary
	} else {
		pcur_module_attr->remote_fd = fd;
		PRINTF("GPRS/CDMA module model: %s\n", pcur_module_attr->describe);
		if (NULL == pcur_module_attr->init) { /// if not assign module-init-function/method in module information struct
			ret = 0;
		} else {
			module_st = pcur_module_attr->init(fd);
			if (module_st != e_modem_st_normal) {
				ret = modem_st_to_ret(module_st);
			} else {
				ret = modem_st_to_ret(
						check_network_status(fd, pcur_module_attr));
			}
		}
		PRINTF("Check modem %s(ret: %d)\n", ret > 1 ? "ok" : "fail", ret);
		close_modem_device(lock_name);
	}
	// lcd_update_head_enable(1);
	return ret;
}

static int do_modem_check_baud(const char *device_name, const char *lock_name) {
	int ret, i, fd, lock_pid;
	char cmd[256], req[256];
	struct {
		int baud;
		char *str;
	} baud_test[] = { { MODEM_DEFAULT_BAUD, MODEM_DEFAULT_BAUD_STR }, /// 115200
			{ B57600, "57600" }, { B9600, "9600" }, { B115200, "115200" }, };

	for (i = 0; i < sizeof(baud_test) / sizeof(baud_test[0]) /// for (i=0; i< 4; i++)
	&& !g_terminated; i++) {
		if (lcd_mode_get() == LCD_MODE_TEST)
			return 0;
		PRINTF("try use with %s\n", baud_test[i].str);
		/// PRINTF("%s: try use with %s\n", __FUNCTION__, baud_test[i].str); /// ADD BY WD
		if (!test_modem_cmd(device_name, lock_name)) /// ret = 0, failed
			continue;
		ret = do_modem_check(device_name, lock_name, baud_test[i].baud);
		if (ret > 0) { /// ret > 0, success
			if (!write_lock_file(lock_name)) {
				lock_pid = read_lock_file(lock_name);
				PRINTF("%s is locked by %d\n", device_name, lock_pid);
				return 0;
			}
			if ((fd = open_serial(device_name, baud_test[i].baud, 8, 0)) < 0) {
				remove(lock_name);
				return 0;
			}
			if (fd >= 0) {
				snprintf(req, sizeof(req), "AT+IPR=%s\r",
						MODEM_DEFAULT_BAUD_STR);
				at_cmd(fd, req, cmd, sizeof(cmd), 1000, 500);
				close_serial(fd);
				remove(lock_name);
			} else {
				remove(lock_name);
			}
			PRINTF("End of trying use with %s, ret: %d\n\n", baud_test[i].str,
					ret);
			return ret;
		} else
			/// ret = 0, failed
			break;
	}
	return 0; /// failed when return 0
}

int modem_check(const char *device_name, const char *lock_name, int baudrate) /// serial port operations
{
	static unsigned int interval, last_result;
	static long test_uptime;
	const int max_interval = 15;
	const int min_interval = 5;
	int ret, wait_soft_reset_time = 5;
	static int modem_poweron_ok;

	/* power check, check sample */
	if (!modem_poweron_ok) { /// check power
		wait_delay(wait_soft_reset_time * 1000);
		if (!test_modem_cmd(device_name, lock_name)) {
			modem_soft_reset();
		}
		modem_poweron_ok = 1;
	}

	/* first time test the modem */
	if (test_uptime == 0 || uptime() < test_uptime) {
		// for first time or time is error
		ret = do_modem_check_baud(device_name, lock_name);
		interval = min_interval;
		test_uptime = uptime();
		last_result = ret;
	}
	/* interval less than interval, need no test */
	else if (uptime() - test_uptime <= interval) {
		ret = 0;
	}
	/* need to test modem again /// 正常检查时间之外 */
	else {
		ret = do_modem_check(device_name, lock_name, baudrate);
		if (ret == 0 && last_result == 0) { /// modem_check not ok, 上次 do_modem_check_buad不OK
			modem_soft_reset();
			ret = do_modem_check_baud(device_name, lock_name); /// 还是不OK
			if (ret == 0) { /// failed check baud
				modem_hard_reset();
				wait_delay(10000);
				ret = do_modem_check_baud(device_name, lock_name); /// hard reset and check baud
			}
		}
		if (ret || interval >= max_interval) { /// ret OK, interval > interval = 5s 
			interval = min_interval;
		} else {
			interval++; /// 控制interval 5-15 之间
		}
		test_uptime = uptime(); /// uptime is what
		last_result = ret;
	}
	return (ret > 2) ? 1 : 0; /// if ret > 2, return 1
}

int remote_ppp_connect(const char *device_name, const char *lock_name,
		const char *baudstr) {
	int ret = 0;
	PROTOCOL_STACK *p_pclstack = NULL; /// for referring protocol stack

	if (NULL == pcur_module_attr) /// current module pointer
		return FALSE;
	pcur_module_attr->remote_fd = -1;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_OS) {
		p_pclstack = &pcur_module_attr->pcl_in_os;
		PRINTF("%s use PPP protocol stack in OS\n", pcur_module_attr->describe); /// ethernet connection
	} else {
		PRINTF("%s use PPP protocol stack in module\n",
				pcur_module_attr->describe);
		p_pclstack = &pcur_module_attr->pcl_in_module; /// GPRS/CDMA 
	}
	if (p_pclstack == NULL)
		return FALSE;
	else {
		if (p_pclstack->ppp_connect) {

			// if(pcur_module_attr != NULL && pcur_module_attr->remote_fd > 0){ /// added by wd
			// 	ret = pcur_module_attr->remote_fd;
			// }else{
			ret = p_pclstack->ppp_connect(device_name, lock_name, baudstr); /// can't set pcur_module_attr
			// }

			if (ret > 0) {
				pcur_module_attr->remote_fd = ret;
				PRINTF("%s PPP connect OK\n", pcur_module_attr->describe);
			} else {
				PRINTF("%s PPP connect FAIL\n", pcur_module_attr->describe);
			}
			return ret;
		} else {
			return FALSE;
		}
	}
}

int remote_tcpudp_connect(BYTE type, const char *addr, int port, int timeout) {
	PROTOCOL_STACK *p_pclstack = NULL;
	int (*tcp_fn)(int, const char *, int, int); /* varieble declaration */
	int (*udp_fn)(int, const char *, int);
	int ret = -1;

	if (NULL == pcur_module_attr)
		return -1;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_MODULE) {
		if (pcur_module_attr->remote_fd < 0)
			return -1;
		p_pclstack = &pcur_module_attr->pcl_in_module;
		PRINTF("%s use TCP/IP protocol stack in module\n",
				pcur_module_attr->describe);
	} else {
		p_pclstack = &pcur_module_attr->pcl_in_os;
		PRINTF("%s use TCP/IP protocol stack in OS\n",
				pcur_module_attr->describe);
	}
	if (NULL == p_pclstack)
		return -1;
	tcp_fn = p_pclstack->tcp_connect;
	udp_fn = p_pclstack->udp_connect;
	if (type == 1) { // connect UDP
		if (udp_fn
				&& (ret = udp_fn(pcur_module_attr->remote_fd, addr, port))
						>= 0) {
			pcur_module_attr->remote_fd = ret;
			PRINTF("%s UDP connect OK, addr: %s, port: %d\n",
					pcur_module_attr->describe, addr, port);
			pcur_module_attr->socket_type = MODEM_CONNECT_SOCKET_TYPE_UDP;
			return ret;
		} else {
			PRINTF("%s UDP connect FAIL, addr: %s, port: %d\n",
					pcur_module_attr->describe, addr, port);
			return -1;
		}
	} else { // connect TCP
		if (tcp_fn
				&& (ret = tcp_fn(pcur_module_attr->remote_fd, addr, port,
						timeout)) >= 0) {
			pcur_module_attr->remote_fd = ret;
			PRINTF("%s TCP connect OK, addr: %s, port: %d, fd: %d\n",
					pcur_module_attr->describe, addr, port, ret);
			pcur_module_attr->socket_type = MODEM_CONNECT_SOCKET_TYPE_TCP;
			return ret;
		} else {
			PRINTF("%s TCP connect FAIL, addr: %s, port: %d\n",
					pcur_module_attr->describe, addr, port);
			return -1;
		}
	}
}

int remote_tcpudp_read(int fd, void *buf, int max_len, int timeout,
		int *errcode) {
	PROTOCOL_STACK *p_pclstack = NULL;
	int ret;

	if (NULL == errcode) {
		PRINTF("Call %s arguments error\n", __func__);
		return 0;
	}
	*errcode = REMOTE_MODULE_RW_NORMAL;
	if (NULL == pcur_module_attr)
		return 0;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_OS) { /// OS TYPE
		*errcode = REMOTE_MODULE_UNREAD_PCLSTACKINOS;
		return 0;
	} else { /// MODULE
		p_pclstack = &pcur_module_attr->pcl_in_module;
	}
	if (NULL == p_pclstack)
		return 0;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_OS) {
		ret = p_pclstack->receive(fd, buf, max_len, timeout, errcode); /// cm180_receive
	} else {  /// MODEM_PCL_STACK_IN_MODULE
		ret = p_pclstack->receive(pcur_module_attr->remote_fd, buf, max_len,
				timeout, errcode);
	}
	if (ret > 0) {
		*errcode = REMOTE_MODULE_RW_NORMAL;  /// NORMAL READ AND WRITE
	}/*else{
	 *errcode = REMOTE_MODULE_RW_UNORMAL; /// UNNORMAL READ AND WRITE
	 }*/
	return ret;
}

int remote_tcpudp_write(int fd, const void *buf, int len, int *errcode) {
	PROTOCOL_STACK *p_pclstack = NULL;
	int ret;

	if (NULL == errcode) {
		PRINTF("Call %s arguments error\n", __func__); /// __func__ == remote_tcpudp_write
		return 0;
	}
	*errcode = REMOTE_MODULE_RW_NORMAL;
	if (NULL == pcur_module_attr)
		return 0;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_OS) {
		*errcode = REMOTE_MODULE_UNWRITE_PCLSTACKINOS;
		return 0;
	} else {
		p_pclstack = &pcur_module_attr->pcl_in_module;
	}
	if (NULL == p_pclstack)
		return 0;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_OS) {
		ret = p_pclstack->send(fd, buf, len, errcode);
	} else {
		ret = p_pclstack->send(pcur_module_attr->remote_fd, buf, len, errcode);
	}
	if (ret > 0) {
		*errcode = REMOTE_MODULE_RW_NORMAL;
	}
	return ret;
}

int remote_module_close(const char *lock_name) // remote module close
{
	int fd;

	if (NULL == pcur_module_attr)
		return FALSE;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_MODULE) {
		fd = pcur_module_attr->remote_fd;
		if (fd >= 0) {
			if (pcur_module_attr->pcl_in_module.shutdown) {
				pcur_module_attr->pcl_in_module.shutdown(fd);
			}
			pcur_module_attr->remote_fd = -1;
		}
		close_modem_device(lock_name);
	}
	return TRUE;
}

BOOL remote_module_get_ip(char *des_addr) /// remote ip address
{
	PROTOCOL_STACK *p_pclstack = NULL;

	if (NULL == pcur_module_attr)
		return FALSE;
	if (pcur_module_attr->protocol_stack_type == MODEM_PCL_STACK_IN_OS) {
		p_pclstack = &pcur_module_attr->pcl_in_os;
	} else {
		p_pclstack = &pcur_module_attr->pcl_in_module;
	}
	if (p_pclstack && p_pclstack->getip) {
		return p_pclstack->getip(pcur_module_attr->remote_fd, des_addr); /// getip function
	} else
		return FALSE;
}

int remote_module_get_netinfo(BYTE *type, BYTE *value, BYTE *level) {
	if (pcur_module_attr == NULL) {
		*type = MODEM_NET_TYPE_UNKNOWN;
		return 0;
	}
	*type = pcur_module_attr->net_type;
	if (pcur_module_attr->signal_intensity == 99)
		return 0;
	if (value != NULL)
		*value = pcur_module_attr->signal_intensity;
	if (pcur_module_attr->signal_intensity <= 7)
		*level = 1;
	else if (pcur_module_attr->signal_intensity <= 14)
		*level = 2;
	else if (pcur_module_attr->signal_intensity <= 21)
		*level = 3;
	else
		*level = 4;
	return 1;
}


int module_update_time(char *pulic_time_server) {

	return 0;
}