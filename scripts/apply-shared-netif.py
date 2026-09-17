from pathlib import Path


def rep(path, old, new):
    p = Path(path)
    s = p.read_text()
    n = s.count(old)
    if n != 1:
        raise SystemExit(f"{path}: expected one match, got {n}")
    p.write_text(s.replace(old, new, 1))


rep("src/u-boot/net/lwip/net-lwip.c", '''\tif (eth_start_udev(udev) < 0) {
\t\tlog_err("Could not start %s\\n", udev->name);
\t\treturn NULL;
\t}

\tnetif_remove(net_lwip_get_netif());
''', '''\tif (eth_start_udev(udev) < 0) {
\t\tlog_err("Could not start %s\\n", udev->name);
\t\treturn NULL;
\t}

\t/* Never tear down an already-live lwIP netif implicitly. Long-lived
\t * services such as UrsusWeb own it; borrow-aware ping/TFTP reuse it. */
\tif (net_lwip_get_netif()) {
\t\tprintf("Network busy: active lwIP interface is owned by another service.\\n"
\t\t       "Stop WebFailsafe with Ctrl-C, run the command, then restart with 'ursusweb'.\\n");
\t\treturn NULL;
\t}
''')

rep("src/u-boot/cmd/lwip/ping.c", '''\tstruct ping_ctx ctx = {};
\tstruct netif *netif;
\tint ret;

\tnetif = net_lwip_new_netif(udev);
\tif (!netif)
\t\treturn -ENODEV;
''', '''\tstruct ping_ctx ctx = {};
\tstruct netif *netif;
\tbool borrowed = false;
\tint ret;

\tnetif = net_lwip_get_netif();
\tif (netif) {
\t\tif (netif->state != udev)
\t\t\treturn -EBUSY;
\t\tborrowed = true;
\t} else {
\t\tnetif = net_lwip_new_netif(udev);
\t\tif (!netif)
\t\t\treturn -ENODEV;
\t}
''')
rep("src/u-boot/cmd/lwip/ping.c", '''\tnet_lwip_remove_netif(netif);

\tif (ctx.alive)
''', '''\tif (!borrowed)
\t\tnet_lwip_remove_netif(netif);

\tif (ctx.alive)
''')

rep("src/u-boot/net/lwip/tftp.c", '''\tint blksize = CONFIG_TFTP_BLOCKSIZE;
\tstruct netif *netif;
\tstruct tftp_ctx ctx;
''', '''\tint blksize = CONFIG_TFTP_BLOCKSIZE;
\tstruct netif *netif;
\tbool borrowed = false;
\tstruct tftp_ctx ctx;
''')
rep("src/u-boot/net/lwip/tftp.c", '''\tnetif = net_lwip_new_netif(udev);
\tif (!netif)
\t\treturn -1;

\tctx.done = NOT_DONE;
''', '''\tnetif = net_lwip_get_netif();
\tif (netif) {
\t\tif (netif->state != udev)
\t\t\treturn -EBUSY;
\t\tborrowed = true;
\t} else {
\t\tnetif = net_lwip_new_netif(udev);
\t\tif (!netif)
\t\t\treturn -1;
\t}

\tctx.done = NOT_DONE;
''')
rep("src/u-boot/net/lwip/tftp.c", '''\tif (err != ERR_OK) {
\t\tprintf("tftp_get() error %d\\n", err);
\t\tnet_lwip_remove_netif(netif);
\t\treturn -1;
\t}
''', '''\tif (err != ERR_OK) {
\t\tprintf("tftp_get() error %d\\n", err);
\t\tif (!borrowed)
\t\t\tnet_lwip_remove_netif(netif);
\t\treturn -1;
\t}
''')
rep("src/u-boot/net/lwip/tftp.c", '''\ttftp_cleanup();

\tnet_lwip_remove_netif(netif);

\tif (ctx.done == SUCCESS) {
''', '''\ttftp_cleanup();

\tif (!borrowed)
\t\tnet_lwip_remove_netif(netif);

\tif (ctx.done == SUCCESS) {
''')

p = Path("src/u-boot/cmd/ursusweb.c")
s = p.read_text()
marker = '''    bool reboot_after_response;
};

static void ursus_log_reset(void)
'''
insert = '''    bool reboot_after_response;
};

/* Web-console commands are deferred out of the TCP receive callback so a
 * command may safely pump the already-live lwIP netif (ping/TFTP). */
static struct tcp_pcb *ursus_console_pending_pcb;
static struct ursus_conn *ursus_console_pending_conn;
static char ursus_console_pending_cmd[256];

static void ursus_console_pending_clear(struct ursus_conn *c)
{
    if (!c || ursus_console_pending_conn == c) {
        ursus_console_pending_pcb = NULL;
        ursus_console_pending_conn = NULL;
        ursus_console_pending_cmd[0] = 0;
    }
}

static void ursus_log_reset(void)
'''
if s.count(marker) != 1:
    raise SystemExit("ursusweb: struct marker mismatch")
p.write_text(s.replace(marker, insert, 1))

rep("src/u-boot/cmd/ursusweb.c", '''    free(c);
    if (stop)
        ursus_stop = true;
''', '''    ursus_console_pending_clear(c);
    free(c);
    if (stop)
        ursus_stop = true;
''')
rep("src/u-boot/cmd/ursusweb.c", '''static void ursus_http_err(void *arg, err_t err)
{
    struct ursus_conn *c = arg;
    printf("URSUS_HTTP_ERROR conn=%u err=%d\\n", c ? c->id : 0, err);
    free(c);
}
''', '''static void ursus_http_err(void *arg, err_t err)
{
    struct ursus_conn *c = arg;
    printf("URSUS_HTTP_ERROR conn=%u err=%d\\n", c ? c->id : 0, err);
    ursus_console_pending_clear(c);
    free(c);
}
''')
rep("src/u-boot/cmd/ursusweb.c", '''        ursus_console_capture(cmd);
        return ursus_http_start_response(pcb, c, 200, "text/plain; charset=utf-8", ursus_console_body);
''', '''        if (ursus_console_pending_conn)
            return ursus_http_start_response(pcb, c, 409, "text/plain; charset=utf-8",
                "another Web console command is still running\\n");
        ursus_console_pending_pcb = pcb;
        ursus_console_pending_conn = c;
        snprintf(ursus_console_pending_cmd, sizeof(ursus_console_pending_cmd), "%s", cmd);
        printf("URSUS_CONSOLE_DEFER cmd=%s\\n", cmd);
        return ERR_OK;
''')

p = Path("src/u-boot/cmd/ursusweb.c")
s = p.read_text()
marker = "static int ursus_console_capture_selftest(void)\n"
helper = '''static void ursus_console_service_pending(void)
{
    struct tcp_pcb *pcb = ursus_console_pending_pcb;
    struct ursus_conn *c = ursus_console_pending_conn;
    char cmd[sizeof(ursus_console_pending_cmd)];
    err_t err;

    if (!pcb || !c || !ursus_console_pending_cmd[0])
        return;

    snprintf(cmd, sizeof(cmd), "%s", ursus_console_pending_cmd);
    ursus_console_capture(cmd);

    if (ursus_console_pending_pcb != pcb || ursus_console_pending_conn != c)
        return;

    ursus_console_pending_pcb = NULL;
    ursus_console_pending_conn = NULL;
    ursus_console_pending_cmd[0] = 0;
    err = ursus_http_start_response(pcb, c, 200, "text/plain; charset=utf-8",
                                    ursus_console_body);
    if (err != ERR_OK)
        ursus_conn_release(pcb, c, true);
}

static int ursus_console_capture_selftest(void)
'''
if s.count(marker) != 1:
    raise SystemExit("ursusweb: console selftest marker mismatch")
p.write_text(s.replace(marker, helper, 1))

rep("src/u-boot/cmd/ursusweb.c", '''        if (ch == 3) {
            printf("^C\\n");
            ursus_uart_line_len = 0;
            ursus_uart_shell_prompt();
            continue;
        }
''', '''        if (ch == 3) {
            printf("^C\\nURSUS_WEB_STOP_REQUEST source=UART\\n");
            ursus_uart_line_len = 0;
            ursus_stop = true;
            return;
        }
''')
rep("src/u-boot/cmd/ursusweb.c", '''        ursus_uart_shell_poll();
        ret = net_lwip_rx(ursus_web_udev, netif);
        ursus_rx_report(ret);
        sys_check_timeouts();
''', '''        ursus_uart_shell_poll();
        if (ursus_stop)
            break;
        ret = net_lwip_rx(ursus_web_udev, netif);
        ursus_rx_report(ret);
        ursus_console_service_pending();
        sys_check_timeouts();
''')
rep("src/u-boot/cmd/ursusweb.c", '''    ursus_web_udev = NULL;
    ursus_web_running = false;

    if (ursus_network_failed)
''', '''    ursus_web_udev = NULL;
    ursus_web_running = false;
    ursus_console_pending_clear(NULL);
    printf("URSUS_WEB_STOPPED restart=ursusweb\\n");

    if (ursus_network_failed)
''')

q = Path("scripts/qa.sh")
qs = q.read_text()
old = '''test -d "$ROOT/src/u-boot"; grep -q 'Repository policy: self-contained' "$ROOT/README.md"; echo URSUSBOOT_STANDALONE_QA=PASS
'''
new = '''test -d "$ROOT/src/u-boot"
grep -q 'Repository policy: self-contained' "$ROOT/README.md"
grep -q 'bool borrowed = false' "$ROOT/src/u-boot/cmd/lwip/ping.c"
grep -q 'bool borrowed = false' "$ROOT/src/u-boot/net/lwip/tftp.c"
grep -q 'URSUS_CONSOLE_DEFER' "$ROOT/src/u-boot/cmd/ursusweb.c"
grep -q 'URSUS_WEB_STOP_REQUEST source=UART' "$ROOT/src/u-boot/cmd/ursusweb.c"
grep -q 'Network busy: active lwIP interface' "$ROOT/src/u-boot/net/lwip/net-lwip.c"
echo URSUSBOOT_STANDALONE_QA=PASS
'''
if qs.count(old) != 1:
    raise SystemExit("qa marker mismatch")
q.write_text(qs.replace(old, new, 1))
