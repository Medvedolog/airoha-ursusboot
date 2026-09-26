/* SPDX-License-Identifier: GPL-2.0+
 *
 * Experimental UrsusBoot WebSocket console transport.
 *
 * This is intentionally kept separate from /api/console.  The HTTP console
 * remains the stateless command RPC; this transport exposes the existing
 * Ursus WebFailsafe shell as a byte stream and mirrors the physical serial
 * console instead of replacing it.
 */

#define URSUS_WS_RX_SIZE       2048
#define URSUS_WS_TX_SIZE       16384
#define URSUS_WS_FRAME_MAX     2048
#define URSUS_WS_TX_CHUNK      512
#define URSUS_WS_GUID          "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define URSUS_WS_PROTOCOL      "ursusboot-console-v1"
#define URSUS_WS_POLL_MS       5UL


static const char ursus_ws_page[] =
"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>UrsusBoot Live Console</title><style>body{margin:0;background:#171009;color:#f2e8da;font:14px system-ui}"
"main{max-width:1050px;margin:auto;padding:16px}h1{font-size:20px}.bar{display:flex;gap:8px;align-items:center;margin:10px 0}"
"button{background:#d99a4d;border:0;border-radius:7px;padding:8px 12px;font-weight:700;cursor:pointer}"
"#st{color:#a89179}pre{height:70vh;overflow:auto;background:#120c07;border:1px solid #4a3728;border-radius:9px;padding:12px;"
"white-space:pre-wrap;word-break:break-word;font:12px/1.4 ui-monospace,SFMono-Regular,Consolas,monospace;outline:none}"
".warn{color:#d6a76b;font-size:12px}</style></head><body><main><h1>UrsusBoot · Live UART/WebSocket console</h1>"
"<div class=\"bar\"><button id=\"c\">Connect</button><span id=\"st\">disconnected</span></div>"
"<div class=\"warn\">EXPERT: real U-Boot stdin/stdout/stderr. Physical UART stays active. Flash commands are not filtered.</div>"
"<pre id=\"o\" tabindex=\"0\">Click Connect, then focus this terminal.\n</pre>"
"<script>let w=null,hist=[],hp=0,line='';const o=document.getElementById('o'),st=document.getElementById('st'),b=document.getElementById('c');"
"const dec=new TextDecoder();function wr(s){s=s.replace(/\\x1b\\[[0-9;?]*[ -\\/]*[@-~]/g,'');let t=o.textContent;"
"for(const ch of s){if(ch==='\\b')t=t.slice(0,-1);else if(ch==='\\r'){}else t+=ch}o.textContent=t;o.scrollTop=o.scrollHeight}"
"function send(s){if(w&&w.readyState===1)w.send(s)}function connect(){if(w){w.close();return}const p=location.protocol==='https:'?'wss://':'ws://';"
"w=new WebSocket(p+location.host+'/ws/console','ursusboot-console-v1');w.binaryType='arraybuffer';st.textContent='connecting';"
"w.onopen=()=>{st.textContent='connected';b.textContent='Disconnect';o.focus()};"
"w.onmessage=e=>wr(typeof e.data==='string'?e.data:dec.decode(e.data));"
"w.onerror=()=>{st.textContent='error'};w.onclose=()=>{w=null;st.textContent='disconnected';b.textContent='Connect'}}"
"b.onclick=connect;o.addEventListener('keydown',e=>{if(!w||w.readyState!==1)return;let s=null;"
"if(e.ctrlKey&&(e.key==='c'||e.key==='C'))s='\\x03';else if(e.ctrlKey&&(e.key==='u'||e.key==='U')){s='\\x15';line='';hp=hist.length}"
"else if(e.key==='ArrowUp'){if(hist.length){hp=Math.max(0,hp-1);line=hist[hp]||'';s='\\x15'+line}}"
"else if(e.key==='ArrowDown'){if(hist.length){hp=Math.min(hist.length,hp+1);line=hp<hist.length?hist[hp]:'';s='\\x15'+line}}"
"else if(e.key==='Enter'){if(line&&hist[hist.length-1]!==line)hist.push(line);hp=hist.length;line='';s='\\r'}"
"else if(e.key==='Backspace'){line=line.slice(0,-1);s='\\x08'}else if(e.key==='Tab')s='\\t';"
"else if(e.key.length===1&&!e.altKey&&!e.metaKey){line+=e.key;s=e.key}if(s!==null){e.preventDefault();send(s)}});"
"o.addEventListener('paste',e=>{if(!w||w.readyState!==1)return;e.preventDefault();let t=e.clipboardData.getData('text');if(!/[\\r\\n]/.test(t))line+=t;else line='';send(t.replace(/\\r?\\n/g,'\\r'))});"
"</script></main></body></html>";

struct ursus_ws_state {
    bool active;
    bool stdio_attached;
    bool close_requested;
    struct tcp_pcb *pcb;
    struct ursus_conn *conn;

    struct stdio_dev *orig_in;
    struct stdio_dev *orig_out;
    struct stdio_dev *orig_err;
    char orig_in_name[STDIO_NAME_LEN];
    char orig_out_name[STDIO_NAME_LEN];
    char orig_err_name[STDIO_NAME_LEN];

    u8 rx[URSUS_WS_RX_SIZE];
    size_t rx_head;
    size_t rx_tail;
    u8 tx[URSUS_WS_TX_SIZE];
    size_t tx_head;
    size_t tx_tail;
    unsigned int tx_dropped;

    u8 hdr[14];
    size_t hdr_len;
    size_t hdr_need;
    u8 opcode;
    bool fin;
    bool masked;
    u64 payload_len;
    u64 payload_got;
    u8 mask[4];
    u8 payload[URSUS_WS_FRAME_MAX];
};

static struct ursus_ws_state ursus_ws;
static bool ursus_ws_stdio_registered;
static bool ursus_ws_pumping;
static bool ursus_schedule_pumping;
static ulong ursus_ws_last_schedule_poll;

static size_t ursus_ws_ring_used(size_t head, size_t tail, size_t size)
{
    return head >= tail ? head - tail : size - tail + head;
}

static bool ursus_ws_rx_push(u8 ch)
{
    size_t next = (ursus_ws.rx_head + 1) % sizeof(ursus_ws.rx);

    if (next == ursus_ws.rx_tail)
        return false;
    ursus_ws.rx[ursus_ws.rx_head] = ch;
    ursus_ws.rx_head = next;
    return true;
}

static int ursus_ws_rx_pop(void)
{
    int ch;

    if (ursus_ws.rx_head == ursus_ws.rx_tail)
        return -1;
    ch = ursus_ws.rx[ursus_ws.rx_tail];
    ursus_ws.rx_tail = (ursus_ws.rx_tail + 1) % sizeof(ursus_ws.rx);
    return ch;
}

static void ursus_ws_tx_push(u8 ch)
{
    size_t next;

    if (!ursus_ws.active)
        return;
    next = (ursus_ws.tx_head + 1) % sizeof(ursus_ws.tx);
    if (next == ursus_ws.tx_tail) {
        /*
         * Never silently corrupt a live terminal transcript.  The physical
         * UART still receives the full output; terminate only the WebSocket
         * side and let the operator reconnect.
         */
        ursus_ws.tx_dropped++;
        ursus_ws.close_requested = true;
        return;
    }
    ursus_ws.tx[ursus_ws.tx_head] = ch;
    ursus_ws.tx_head = next;
}

static size_t ursus_ws_base64(const u8 *src, size_t len, char *dst, size_t dst_len)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;

    while (i < len) {
        u32 v = (u32)src[i++] << 16;
        bool have2 = i < len;
        bool have3;

        if (have2)
            v |= (u32)src[i++] << 8;
        have3 = i < len;
        if (have3)
            v |= src[i++];

        if (o + 4 >= dst_len)
            return 0;
        dst[o++] = tab[(v >> 18) & 0x3f];
        dst[o++] = tab[(v >> 12) & 0x3f];
        dst[o++] = have2 ? tab[(v >> 6) & 0x3f] : '=';
        dst[o++] = have3 ? tab[v & 0x3f] : '=';
    }
    if (o >= dst_len)
        return 0;
    dst[o] = 0;
    return o;
}

static err_t ursus_ws_send_frame(u8 opcode, const u8 *payload, size_t len)
{
    u8 frame[4 + URSUS_WS_TX_CHUNK];
    size_t hlen;
    err_t err;

    if (!ursus_ws.active || !ursus_ws.pcb)
        return ERR_CONN;
    if (len > URSUS_WS_TX_CHUNK)
        return ERR_VAL;

    frame[0] = 0x80 | (opcode & 0x0f);
    if (len <= 125) {
        frame[1] = len;
        hlen = 2;
    } else {
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xff;
        frame[3] = len & 0xff;
        hlen = 4;
    }
    if (tcp_sndbuf(ursus_ws.pcb) < hlen + len)
        return ERR_MEM;
    if (len)
        memcpy(frame + hlen, payload, len);
    err = tcp_write(ursus_ws.pcb, frame, hlen + len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
        return err;
    return tcp_output(ursus_ws.pcb);
}

static void ursus_ws_flush_tx(void)
{
    u8 payload[URSUS_WS_TX_CHUNK];

    while (ursus_ws.active && ursus_ws.tx_head != ursus_ws.tx_tail) {
        size_t used = ursus_ws_ring_used(ursus_ws.tx_head, ursus_ws.tx_tail,
                                         sizeof(ursus_ws.tx));
        size_t n = min_t(size_t, used, sizeof(payload));
        size_t i;
        err_t err;

        for (i = 0; i < n; i++)
            payload[i] = ursus_ws.tx[(ursus_ws.tx_tail + i) % sizeof(ursus_ws.tx)];
        err = ursus_ws_send_frame(0x2, payload, n);
        if (err == ERR_MEM)
            return;
        if (err != ERR_OK) {
            ursus_ws.close_requested = true;
            return;
        }
        ursus_ws.tx_tail = (ursus_ws.tx_tail + n) % sizeof(ursus_ws.tx);
    }
}

static void ursus_ws_network_pump(void)
{
    if (ursus_ws_pumping)
        return;
    ursus_ws_pumping = true;
    if (ursus_web_udev && ursus_web_netif)
        net_lwip_rx(ursus_web_udev, ursus_web_netif);
    sys_check_timeouts();
    ursus_ws_flush_tx();
    ursus_led_poll();
    ursus_ws_pumping = false;
}

/* schedule() is already called between SPI-NAND pages/eraseblocks.  While the
 * live WebSocket console owns the control plane, turn those existing yields
 * into a throttled lwIP service point without enabling CONFIG_CYCLIC. */
void board_schedule_poll(void)
{
    if (!ursus_web_running || ursus_ws_pumping || ursus_schedule_pumping ||
        !ursus_web_udev || !ursus_web_netif)
        return;
    if (get_timer(ursus_ws_last_schedule_poll) < URSUS_WS_POLL_MS)
        return;

    /*
     * schedule() is already reached between SPI-NAND pages/eraseblocks and
     * from UBI's lower MTD I/O. Service lwIP there so TCP ACKs/timers keep
     * moving during long flash work. New HTTP control requests are rejected
     * while this cooperative poll is active to avoid re-entering UBI/status.
     */
    ursus_ws_last_schedule_poll = get_timer(0);
    ursus_schedule_pumping = true;
    ursus_ws_network_pump();
    ursus_schedule_pumping = false;
}

static bool ursus_schedule_poll_active(void)
{
    return ursus_schedule_pumping;
}

static void ursus_ws_stdio_putc(struct stdio_dev *dev, const char ch)
{
    if (ursus_ws.orig_out && ursus_ws.orig_out != dev && ursus_ws.orig_out->putc)
        ursus_ws.orig_out->putc(ursus_ws.orig_out, ch);

    ursus_ws_tx_push((u8)ch);
    if (ch == '\n' ||
        ursus_ws_ring_used(ursus_ws.tx_head, ursus_ws.tx_tail,
                           sizeof(ursus_ws.tx)) >= 256)
        ursus_ws_flush_tx();

    if (ursus_ws_ring_used(ursus_ws.tx_head, ursus_ws.tx_tail,
                           sizeof(ursus_ws.tx)) > sizeof(ursus_ws.tx) / 2)
        ursus_ws_network_pump();
}

static void ursus_ws_stdio_puts(struct stdio_dev *dev, const char *s)
{
    const char *p;

    if (ursus_ws.orig_out && ursus_ws.orig_out != dev) {
        if (ursus_ws.orig_out->puts)
            ursus_ws.orig_out->puts(ursus_ws.orig_out, s);
        else if (ursus_ws.orig_out->putc)
            for (p = s; *p; p++)
                ursus_ws.orig_out->putc(ursus_ws.orig_out, *p);
    }

    for (p = s; *p; p++)
        ursus_ws_tx_push((u8)*p);
    ursus_ws_flush_tx();
    if (ursus_ws_ring_used(ursus_ws.tx_head, ursus_ws.tx_tail,
                           sizeof(ursus_ws.tx)) > sizeof(ursus_ws.tx) / 2)
        ursus_ws_network_pump();
}

static int ursus_ws_stdio_tstc(struct stdio_dev *dev)
{
    if (ursus_ws.orig_in && ursus_ws.orig_in != dev && ursus_ws.orig_in->tstc &&
        ursus_ws.orig_in->tstc(ursus_ws.orig_in) > 0)
        return 1;
    if (ursus_ws.rx_head != ursus_ws.rx_tail)
        return 1;
    if (ursus_ws.active)
        ursus_ws_network_pump();
    if (ursus_ws.orig_in && ursus_ws.orig_in != dev && ursus_ws.orig_in->tstc &&
        ursus_ws.orig_in->tstc(ursus_ws.orig_in) > 0)
        return 1;
    return ursus_ws.rx_head != ursus_ws.rx_tail;
}

static int ursus_ws_stdio_getc(struct stdio_dev *dev)
{
    for (;;) {
        int ch;

        if (ursus_ws.orig_in && ursus_ws.orig_in != dev && ursus_ws.orig_in->tstc &&
            ursus_ws.orig_in->tstc(ursus_ws.orig_in) > 0 && ursus_ws.orig_in->getc)
            return ursus_ws.orig_in->getc(ursus_ws.orig_in);

        ch = ursus_ws_rx_pop();
        if (ch >= 0)
            return ch;

        if (!ursus_ws.active) {
            if (ursus_ws.orig_in && ursus_ws.orig_in != dev && ursus_ws.orig_in->getc)
                return ursus_ws.orig_in->getc(ursus_ws.orig_in);
            return -1;
        }
        ursus_ws_network_pump();
        udelay(500);
    }
}

static int ursus_ws_register_stdio(void)
{
    struct stdio_dev dev;

    if (ursus_ws_stdio_registered || stdio_get_by_name("ursusws")) {
        ursus_ws_stdio_registered = true;
        return 0;
    }
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "ursusws");
    dev.flags = DEV_FLAGS_INPUT | DEV_FLAGS_OUTPUT;
    dev.putc = ursus_ws_stdio_putc;
    dev.puts = ursus_ws_stdio_puts;
    dev.getc = ursus_ws_stdio_getc;
    dev.tstc = ursus_ws_stdio_tstc;
    if (stdio_register(&dev))
        return -EIO;
    ursus_ws_stdio_registered = true;
    return 0;
}

static int ursus_ws_attach_stdio(void)
{
    int ret;

    if (ursus_ws.stdio_attached)
        return 0;
    ret = ursus_ws_register_stdio();
    if (ret)
        return ret;

    ursus_ws.orig_in = stdio_devices[stdin];
    ursus_ws.orig_out = stdio_devices[stdout];
    ursus_ws.orig_err = stdio_devices[stderr];
    if (!ursus_ws.orig_in || !ursus_ws.orig_out || !ursus_ws.orig_err)
        return -ENODEV;

    snprintf(ursus_ws.orig_in_name, sizeof(ursus_ws.orig_in_name), "%s",
             ursus_ws.orig_in->name);
    snprintf(ursus_ws.orig_out_name, sizeof(ursus_ws.orig_out_name), "%s",
             ursus_ws.orig_out->name);
    snprintf(ursus_ws.orig_err_name, sizeof(ursus_ws.orig_err_name), "%s",
             ursus_ws.orig_err->name);

    if (console_assign(stdin, "ursusws") < 0)
        return -EIO;
    if (console_assign(stdout, "ursusws") < 0) {
        console_assign(stdin, ursus_ws.orig_in_name);
        return -EIO;
    }
    if (console_assign(stderr, "ursusws") < 0) {
        console_assign(stdout, ursus_ws.orig_out_name);
        console_assign(stdin, ursus_ws.orig_in_name);
        return -EIO;
    }
    ursus_ws.stdio_attached = true;
    return 0;
}

static void ursus_ws_restore_stdio(void)
{
    if (!ursus_ws.stdio_attached)
        return;
    console_assign(stderr, ursus_ws.orig_err_name);
    console_assign(stdout, ursus_ws.orig_out_name);
    console_assign(stdin, ursus_ws.orig_in_name);
    ursus_ws.stdio_attached = false;
}

static void ursus_ws_detach(struct ursus_conn *c)
{
    if (c && ursus_ws.conn && ursus_ws.conn != c)
        return;

    ursus_ws.active = false;
    ursus_ws.close_requested = false;
    ursus_ws_restore_stdio();
    ursus_ws.pcb = NULL;
    ursus_ws.conn = NULL;
    ursus_ws.rx_head = ursus_ws.rx_tail = 0;
    ursus_ws.tx_head = ursus_ws.tx_tail = 0;
    ursus_ws.hdr_len = 0;
    ursus_ws.hdr_need = 2;
}

static bool ursus_ws_is_active(void)
{
    return ursus_ws.active;
}

static void ursus_ws_protocol_close(u16 code)
{
    u8 payload[2] = { (u8)(code >> 8), (u8)code };

    if (ursus_ws.active)
        ursus_ws_send_frame(0x8, payload, sizeof(payload));
    ursus_ws.close_requested = true;
}

static err_t ursus_ws_process_frame(void)
{
    size_t i;

    if (!ursus_ws.fin) {
        ursus_ws_protocol_close(1002);
        return ERR_VAL;
    }

    switch (ursus_ws.opcode) {
    case 0x1:
    case 0x2:
        for (i = 0; i < (size_t)ursus_ws.payload_len; i++) {
            if (!ursus_ws_rx_push(ursus_ws.payload[i])) {
                ursus_ws_protocol_close(1009);
                return ERR_MEM;
            }
        }
        break;
    case 0x8:
        if (ursus_ws.payload_len)
            ursus_ws_send_frame(0x8, ursus_ws.payload,
                                min_t(size_t, ursus_ws.payload_len, 125));
        else
            ursus_ws_protocol_close(1000);
        ursus_ws.close_requested = true;
        break;
    case 0x9:
        if (ursus_ws.payload_len <= 125)
            ursus_ws_send_frame(0xA, ursus_ws.payload, ursus_ws.payload_len);
        else
            ursus_ws_protocol_close(1002);
        break;
    case 0xA:
        break;
    default:
        ursus_ws_protocol_close(1003);
        return ERR_VAL;
    }
    return ERR_OK;
}

static void ursus_ws_reset_frame(void)
{
    ursus_ws.hdr_len = 0;
    ursus_ws.hdr_need = 2;
    ursus_ws.opcode = 0;
    ursus_ws.fin = false;
    ursus_ws.masked = false;
    ursus_ws.payload_len = 0;
    ursus_ws.payload_got = 0;
}

static err_t ursus_ws_feed(const u8 *data, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        if (ursus_ws.hdr_len < ursus_ws.hdr_need) {
            ursus_ws.hdr[ursus_ws.hdr_len++] = data[pos++];
            if (ursus_ws.hdr_len == 2) {
                u8 l = ursus_ws.hdr[1] & 0x7f;

                if (ursus_ws.hdr[0] & 0x70) {
                    ursus_ws_protocol_close(1002);
                    return ERR_VAL;
                }
                ursus_ws.fin = !!(ursus_ws.hdr[0] & 0x80);
                ursus_ws.opcode = ursus_ws.hdr[0] & 0x0f;
                ursus_ws.masked = !!(ursus_ws.hdr[1] & 0x80);
                if (!ursus_ws.masked) {
                    ursus_ws_protocol_close(1002);
                    return ERR_VAL;
                }
                ursus_ws.hdr_need = 2 + (l == 126 ? 2 : l == 127 ? 8 : 0) + 4;
            }
            if (ursus_ws.hdr_len < ursus_ws.hdr_need)
                continue;

            {
                u8 l = ursus_ws.hdr[1] & 0x7f;
                size_t off = 2;
                u64 plen = 0;
                int i;

                if (l < 126) {
                    plen = l;
                } else if (l == 126) {
                    plen = ((u64)ursus_ws.hdr[2] << 8) | ursus_ws.hdr[3];
                    off += 2;
                } else {
                    for (i = 0; i < 8; i++)
                        plen = (plen << 8) | ursus_ws.hdr[2 + i];
                    off += 8;
                }
                if (plen > URSUS_WS_FRAME_MAX ||
                    ((ursus_ws.opcode & 0x8) && plen > 125)) {
                    ursus_ws_protocol_close(1009);
                    return ERR_VAL;
                }
                ursus_ws.payload_len = plen;
                memcpy(ursus_ws.mask, ursus_ws.hdr + off, 4);
                if (!plen) {
                    err_t err = ursus_ws_process_frame();
                    ursus_ws_reset_frame();
                    if (err != ERR_OK)
                        return err;
                    continue;
                }
            }
        }

        while (pos < len && ursus_ws.payload_got < ursus_ws.payload_len) {
            u8 ch = data[pos++] ^
                    ursus_ws.mask[ursus_ws.payload_got & 3];

            ursus_ws.payload[ursus_ws.payload_got++] = ch;
        }
        if (ursus_ws.payload_got == ursus_ws.payload_len) {
            err_t err = ursus_ws_process_frame();
            ursus_ws_reset_frame();
            if (err != ERR_OK)
                return err;
        }
    }
    return ERR_OK;
}

static err_t ursus_ws_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    struct ursus_conn *c = arg;

    if (!c || !c->websocket || ursus_ws.pcb != pcb)
        return ERR_OK;
    ursus_ws_flush_tx();
    return ERR_OK;
}

static err_t ursus_ws_poll(void *arg, struct tcp_pcb *pcb)
{
    struct ursus_conn *c = arg;

    if (!c || !c->websocket || ursus_ws.pcb != pcb)
        return ERR_OK;
    ursus_ws_flush_tx();
    if (ursus_ws.close_requested) {
        ursus_conn_release(pcb, c, true);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static bool ursus_ws_origin_ok(const char *req)
{
    char host[96], origin[128], expected[128];
    size_t n;

    /* Browsers always send Origin: enforce same-origin there.  Native clients
     * such as UrsusFlasher may omit Origin and authenticate the transport by
     * the WebSocket subprotocol + machine-readable hello banner instead. */
    if (!ursus_parse_header_value(req, "Origin", origin, sizeof(origin)))
        return true;
    if (!ursus_parse_header_value(req, "Host", host, sizeof(host)))
        return false;

    n = strlen(host);
    if (n > 3 && !strcmp(host + n - 3, ":80"))
        host[n - 3] = '\0';

    if (snprintf(expected, sizeof(expected), "http://%s", host) <= 0)
        return false;
    return !strcmp(origin, expected);
}

static err_t ursus_ws_upgrade(struct tcp_pcb *pcb, struct ursus_conn *c)
{
    char key[96], version[16], upgrade[32], protocol[96];
    char seed[192], accept[48], response[320];
    u8 digest[SHA1_SUM_LEN];
    int n, ret;
    err_t err;

    if (ursus_ws.active)
        return ursus_http_start_response(pcb, c, 409, "text/plain; charset=utf-8",
            "another live WebSocket console is already attached\n");
    if (!ursus_ws_origin_ok(c->reqhdr))
        return ursus_http_start_response(pcb, c, 403, "text/plain; charset=utf-8",
            "WebSocket Origin must match the recovery page Host\n");
    if (!ursus_parse_header_value(c->reqhdr, "Upgrade", upgrade, sizeof(upgrade)) ||
        strcasecmp(upgrade, "websocket") ||
        !ursus_parse_header_value(c->reqhdr, "Sec-WebSocket-Version",
                                  version, sizeof(version)) ||
        strcmp(version, "13") ||
        !ursus_parse_header_value(c->reqhdr, "Sec-WebSocket-Key",
                                  key, sizeof(key)))
        return ursus_http_start_response(pcb, c, 400, "text/plain; charset=utf-8",
            "invalid WebSocket upgrade request\n");

    if (!ursus_parse_header_value(c->reqhdr, "Sec-WebSocket-Protocol",
                                  protocol, sizeof(protocol)) ||
        strcmp(protocol, URSUS_WS_PROTOCOL))
        return ursus_http_start_response(pcb, c, 400, "text/plain; charset=utf-8",
            "Sec-WebSocket-Protocol must be " URSUS_WS_PROTOCOL "\n");

    n = snprintf(seed, sizeof(seed), "%s%s", key, URSUS_WS_GUID);
    if (n <= 0 || n >= sizeof(seed))
        return ERR_VAL;
    sha1_csum_wd((const unsigned char *)seed, n, digest,
                 SHA1_DEF_CHUNK_SZ);
    if (!ursus_ws_base64(digest, sizeof(digest), accept, sizeof(accept)))
        return ERR_VAL;

    memset(&ursus_ws, 0, sizeof(ursus_ws));
    ursus_ws.hdr_need = 2;
    ursus_ws.pcb = pcb;
    ursus_ws.conn = c;
    ret = ursus_ws_attach_stdio();
    if (ret) {
        ursus_ws_detach(c);
        return ursus_http_start_response(pcb, c, 409, "text/plain; charset=utf-8",
            "could not attach WebSocket console stdio\n");
    }

    n = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "Sec-WebSocket-Protocol: " URSUS_WS_PROTOCOL "\r\n"
        "Cache-Control: no-store\r\n\r\n", accept);
    if (n <= 0 || n >= sizeof(response) || tcp_sndbuf(pcb) < n) {
        ursus_ws_detach(c);
        return ERR_MEM;
    }
    err = tcp_write(pcb, response, n, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        ursus_ws_detach(c);
        return err;
    }
    err = tcp_output(pcb);
    if (err != ERR_OK) {
        ursus_ws_detach(c);
        return err;
    }

    c->websocket = true;
    ursus_ws.active = true;
    tcp_sent(pcb, ursus_ws_sent);
    tcp_poll(pcb, ursus_ws_poll, 1);

    {
        const char *hello =
            "URSUS_WS_HELLO protocol=1 product=UrsusBoot transport=live-stdio\r\n"
            "[UrsusBoot live console attached; physical UART remains active]\r\n"
            "UrsusBoot> ";
        while (*hello)
            ursus_ws_tx_push((u8)*hello++);
        ursus_ws_flush_tx();
    }
    return ERR_OK;
}

static void ursus_ws_service(void)
{
    struct tcp_pcb *pcb;
    struct ursus_conn *c;

    if (!ursus_ws.active)
        return;
    ursus_ws_flush_tx();
    if (!ursus_ws.close_requested)
        return;

    pcb = ursus_ws.pcb;
    c = ursus_ws.conn;
    if (pcb && c)
        ursus_conn_release(pcb, c, true);
    else
        ursus_ws_detach(NULL);
}

static void ursus_ws_shutdown(void)
{
    struct tcp_pcb *pcb = ursus_ws.pcb;
    struct ursus_conn *c = ursus_ws.conn;

    if (ursus_ws.active && pcb)
        ursus_ws_protocol_close(1001);
    if (pcb && c) {
        ursus_conn_release(pcb, c, true);
        return;
    }
    ursus_ws_detach(NULL);
}
