// server.cpp: little more than enhanced multicaster
// runs dedicated or as client coroutine

#include "inexor/engine/engine.hpp"
#include "inexor/crashreporter/CrashReporter.hpp"
#include "inexor/util/Logging.hpp"
#include "inexor/network/legacy/game_types.hpp"
#include "inexor/shared/cube_queue.hpp"
#include "inexor/util/legacy_time.hpp"

#define LOGSTRLEN 512

const char *initscript = NULL;

#ifdef STANDALONE

void conline(int type, const char *sf) {};

void cleanupserver();
void fatal(const char *fmt, ...)
{
    cleanupserver(); 
	defvformatstring(msg,fmt,fmt);
	spdlog::get("global")->critical(msg);
#ifdef WIN32
	MessageBox(NULL, msg, "Inexor fatal error", MB_OK|MB_SYSTEMMODAL);
#else
    fprintf(stderr, "server error: %s\n", msg);
#endif
    exit(EXIT_FAILURE); 
}

/// Fatal crash: log/display crash message and clean up server.
void fatal(std::vector<std::string> &output)
{
    cleanupserver();
    std::string completeoutput;
    for(auto message : output) {
        spdlog::get("global")->critical(message);
        completeoutput = inexor::util::fmt << completeoutput << message.c_str();
    }
#ifdef WIN32
    MessageBox(NULL, completeoutput.c_str(), "Inexor fatal error", MB_OK | MB_SYSTEMMODAL);
#else
    fprintf(stderr, "server error: %s\n", completeoutput.c_str());
#endif
    exit(EXIT_FAILURE);
}

#endif

#define DEFAULTCLIENTS 8

enum { ST_EMPTY, ST_LOCAL, ST_TCPIP };

struct client                   // server side version of "dynent" type
{
    int type;
    int num;
    ENetPeer *peer;
    string hostname;
    void *info;
};

vector<client *> clients;

ENetHost *serverhost = NULL;
int laststatus = 0; 
ENetSocket pongsock = ENET_SOCKET_NULL, lansock = ENET_SOCKET_NULL;

int localclients = 0, nonlocalclients = 0;

bool hasnonlocalclients() { return nonlocalclients!=0; }
bool haslocalclients() { return localclients!=0; }

client &addclient(int type)
{
    client *c = NULL;
    loopv(clients) if(clients[i]->type==ST_EMPTY)
    {
        c = clients[i];
        break;
    }
    if(!c)
    {
        c = new client;
        c->num = clients.length();
        clients.add(c);
    }
    c->info = server::newclientinfo();
    c->type = type;
    switch(type)
    {
        case ST_TCPIP: nonlocalclients++; break;
        case ST_LOCAL: localclients++; break;
    }
    return *c;
}

void delclient(client *c)
{
    if(!c) return;
    switch(c->type)
    {
        case ST_TCPIP: nonlocalclients--; if(c->peer) c->peer->data = NULL; break;
        case ST_LOCAL: localclients--; break;
        case ST_EMPTY: return;
    }
    c->type = ST_EMPTY;
    c->peer = NULL;
    if(c->info)
    {
        server::deleteclientinfo(c->info);
        c->info = NULL;
    }
}

void cleanupserver()
{
    if(serverhost) enet_host_destroy(serverhost);
    serverhost = NULL;

    if(pongsock != ENET_SOCKET_NULL) enet_socket_destroy(pongsock);
    if(lansock != ENET_SOCKET_NULL) enet_socket_destroy(lansock);
    pongsock = lansock = ENET_SOCKET_NULL;
}

VARF(maxclients, 0, DEFAULTCLIENTS, MAXCLIENTS, { if(!maxclients) maxclients = DEFAULTCLIENTS; });
VARF(maxdupclients, 0, 0, MAXCLIENTS, { if(serverhost) serverhost->duplicatePeers = maxdupclients ? maxdupclients : MAXCLIENTS; });

void process(ENetPacket *packet, int sender, int chan);
//void disconnect_client(int n, int reason);

int getservermtu() { return serverhost ? serverhost->mtu : -1; }
void *getclientinfo(int i) { return !clients.inrange(i) || clients[i]->type==ST_EMPTY ? NULL : clients[i]->info; }
ENetPeer *getclientpeer(int i) { return clients.inrange(i) && clients[i]->type==ST_TCPIP ? clients[i]->peer : NULL; }
int getnumclients()        { return clients.length(); }
uint getclientip(int n)    { return clients.inrange(n) && clients[n]->type==ST_TCPIP ? clients[n]->peer->address.host : 0; }

void sendpacket(int n, int chan, ENetPacket *packet, int exclude)
{
    if(n<0)
    {
        server::recordpacket(chan, packet->data, packet->dataLength);
        loopv(clients) if(i!=exclude && server::allowbroadcast(i)) sendpacket(i, chan, packet);
        return;
    }
    switch(clients[n]->type)
    {
        case ST_TCPIP:
        {
            enet_peer_send(clients[n]->peer, chan, packet);
            break;
        }

#ifndef STANDALONE
        case ST_LOCAL:
            localservertoclient(chan, packet);
            break;
#endif
    }
}

ENetPacket *sendf(int cn, int chan, const char *format, ...)
{
    int exclude = -1;
    bool reliable = false;
    if(*format=='r') { reliable = true; ++format; }
    packetbuf p(MAXTRANS, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    va_list args;
    va_start(args, format);
    while(*format) switch(*format++)
    {
        case 'x':
            exclude = va_arg(args, int);
            break;

        case 'v':
        {
            int n = va_arg(args, int);
            int *v = va_arg(args, int *);
            loopi(n) putint(p, v[i]);
            break;
        }

        case 'i': 
        {
            int n = isdigit(*format) ? *format++-'0' : 1;
            loopi(n) putint(p, va_arg(args, int));
            break;
        }
        case 'f':
        {
            int n = isdigit(*format) ? *format++-'0' : 1;
            loopi(n) putfloat(p, (float)va_arg(args, double));
            break;
        }
        case 's': sendstring(va_arg(args, const char *), p); break;
        case 'm':
        {
            int n = va_arg(args, int);
            p.put(va_arg(args, uchar *), n);
            break;
        }
    }
    va_end(args);
    ENetPacket *packet = p.finalize();
    sendpacket(cn, chan, packet, exclude);
    return packet->referenceCount > 0 ? packet : NULL;
}

ENetPacket *sendfile(int cn, int chan, stream *file, const char *format, ...)
{
    if(cn < 0)
    {
#ifdef STANDALONE
        return NULL;
#endif
    }
    else if(!clients.inrange(cn)) return NULL;

    int len = (int)min(file->size(), stream::offset(INT_MAX));
    if(len <= 0 || len > 16<<20) return NULL;

    packetbuf p(MAXTRANS+len, ENET_PACKET_FLAG_RELIABLE);
    va_list args;
    va_start(args, format);
    while(*format) switch(*format++)
    {
        case 'i':
        {
            int n = isdigit(*format) ? *format++-'0' : 1;
            loopi(n) putint(p, va_arg(args, int));
            break;
        }
        case 's': sendstring(va_arg(args, const char *), p); break;
        case 'l': putint(p, len); break;
    }
    va_end(args);

    file->seek(0, SEEK_SET);
    file->read(p.subbuf(len).buf, len);

    ENetPacket *packet = p.finalize();
    if(cn >= 0) sendpacket(cn, chan, packet, -1);
#ifndef STANDALONE
    else sendclientpacket(packet, chan);
#endif
    return packet->referenceCount > 0 ? packet : NULL;
}

const char *disconnectreason(int reason)
{
    switch(reason)
    {
        case DISC_EOP: return "end of packet";
        case DISC_LOCAL: return "server is in local mode";
        case DISC_KICK: return "kicked/banned";
        case DISC_MSGERR: return "message error";
        case DISC_IPBAN: return "ip is banned";
        case DISC_PRIVATE: return "server is in private mode";
        case DISC_MAXCLIENTS: return "server FULL";
        case DISC_TIMEOUT: return "connection timed out";
        case DISC_OVERFLOW: return "overflow";
        case DISC_PASSWORD: return "invalid password";
        default: return NULL;
    }
}

void disconnect_client(int n, int reason)
{
    if(!clients.inrange(n) || clients[n]->type!=ST_TCPIP) return;
    enet_peer_disconnect(clients[n]->peer, reason);
    server::clientdisconnect(n);
    delclient(clients[n]);
    const char *msg = disconnectreason(reason);
    string s;
    if(msg) formatstring(s, "client (%s) disconnected because: %s", clients[n]->hostname, msg);
    else formatstring(s, "client (%s) disconnected", clients[n]->hostname);
    spdlog::get("global")->info(s);
    server::sendservmsg(s);
}

void kicknonlocalclients(int reason)
{
    loopv(clients) if(clients[i]->type==ST_TCPIP) disconnect_client(i, reason);
}

void process(ENetPacket *packet, int sender, int chan)   // sender may be -1
{
    packetbuf p(packet);
    server::parsepacket(sender, chan, p);
    if(p.overread()) { disconnect_client(sender, DISC_EOP); return; }
}

void localclienttoserver(int chan, ENetPacket *packet)
{
    client *c = NULL;
    loopv(clients) if(clients[i]->type==ST_LOCAL) { c = clients[i]; break; }
    if(c) process(packet, c->num, chan);
}

#ifdef STANDALONE
bool resolverwait(const char *name, ENetAddress *address)
{
    return enet_address_set_host(address, name) >= 0;
}

int connectwithtimeout(ENetSocket sock, const char *hostname, const ENetAddress &remoteaddress)
{
    return enet_socket_connect(sock, &remoteaddress);
}
#endif

ENetAddress serveraddress = { ENET_HOST_ANY, ENET_PORT_ANY };

static ENetAddress pongaddr;

void sendserverinforeply(ucharbuf &p)
{
    ENetBuffer buf;
    buf.data = p.buf;
    buf.dataLength = p.length();
    enet_socket_send(pongsock, &pongaddr, &buf, 1);
}

#define MAXPINGDATA 32

void checkserversockets()        // reply all server info requests
{
    static ENetSocketSet readset, writeset;
    ENET_SOCKETSET_EMPTY(readset);
    ENET_SOCKETSET_EMPTY(writeset);
    ENetSocket maxsock = pongsock;
    ENET_SOCKETSET_ADD(readset, pongsock);
    if(lansock != ENET_SOCKET_NULL)
    {
        maxsock = max(maxsock, lansock);
        ENET_SOCKETSET_ADD(readset, lansock);
    }
    if(enet_socketset_select(maxsock, &readset, &writeset, 0) <= 0) return;

    ENetBuffer buf;
    uchar pong[MAXTRANS];
    loopi(2)
    {
        ENetSocket sock = i ? lansock : pongsock;
        if(sock == ENET_SOCKET_NULL || !ENET_SOCKETSET_CHECK(readset, sock)) continue;

        buf.data = pong;
        buf.dataLength = sizeof(pong);
        int len = enet_socket_receive(sock, &pongaddr, &buf, 1);
        if(len < 0 || len > MAXPINGDATA) continue;
        ucharbuf req(pong, len), p(pong, sizeof(pong));
        p.len += len;
        server::serverinforeply(req, p);
    }
}

VAR(serveruprate, 0, 0, INT_MAX);
SVAR(serverip, "");
VARF(serverport, 0, INEXOR_SERVER_PORT, MAX_POSSIBLE_PORT, { if(!serverport) serverport = server_port(); });

void serverslice(bool dedicated, uint timeout)   // main server update, called from main loop in sp, or from below in dedicated server
{
    if(!serverhost) 
    {
        server::serverupdate();
        server::sendpackets();
        return;
    }
       
    // below is network only

    if(dedicated)
#ifdef STANDALONE
        updatetime(server::ispaused(), server::gamespeed);
#else
        updatetime(server::ispaused(), game::gamespeed);
#endif

    server::serverupdate();

    checkserversockets();

    if(totalmillis-laststatus>60*1000)   // display bandwidth stats, useful for server ops
    {
        laststatus = totalmillis;     
        if(nonlocalclients || serverhost->totalSentData || serverhost->totalReceivedData)
            spdlog::get("global")->debug("status: {0} remote clients, {1} send, {2} rec (K/sec)",
                                         nonlocalclients, (serverhost->totalSentData/60.0f/1024), (serverhost->totalReceivedData/60.0f/1024));
        serverhost->totalSentData = serverhost->totalReceivedData = 0;
    }

    ENetEvent event;
    bool serviced = false;
    while(!serviced)
    {
        if(enet_host_check_events(serverhost, &event) <= 0)
        {
            if(enet_host_service(serverhost, &event, timeout) <= 0) break;
            serviced = true;
        }
        switch(event.type)
        {
            case ENET_EVENT_TYPE_CONNECT:
            {
                client &c = addclient(ST_TCPIP);
                c.peer = event.peer;
                c.peer->data = &c;
                string hn;
                copystring(c.hostname, (enet_address_get_host_ip(&c.peer->address, hn, sizeof(hn))==0) ? hn : "unknown");
                spdlog::get("global")->info("client connected ({0})", c.hostname);
                int reason = server::clientconnect(c.num, c.peer->address.host);
                if(reason) disconnect_client(c.num, reason);
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE:
            {
                client *c = (client *)event.peer->data;
                if(c) process(event.packet, c->num, event.channelID);
                if(event.packet->referenceCount==0) enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: 
            {
                client *c = (client *)event.peer->data;
                if(!c) break;
                spdlog::get("global")->info("disconnected client ({0})", c->hostname);
                server::clientdisconnect(c->num);
                delclient(c);
                break;
            }
            default:
                break;
        }
    }
    if(server::sendpackets()) enet_host_flush(serverhost);
}

void flushserver(bool force)
{
    if(server::sendpackets(force) && serverhost) enet_host_flush(serverhost);
}

#ifndef STANDALONE
void localdisconnect(bool cleanup)
{
    bool disconnected = false;
    loopv(clients) if(clients[i]->type==ST_LOCAL) 
    {
        server::localdisconnect(i);
        delclient(clients[i]);
        disconnected = true;
    }
    if(!disconnected) return;
    game::gamedisconnect(cleanup);
    mainmenu = 1;
    // inexor::ui::cef_app->GetUserInterface()->SetMainMenu(true);
}

void localconnect()
{
    client &c = addclient(ST_LOCAL);
    copystring(c.hostname, "local");
    game::gameconnect(false);
    server::localconnect(c.num);
}
#endif

#ifdef WIN32
#include <shellapi.h>

#define IDI_ICON1 1

static string apptip = "";
static HINSTANCE appinstance = NULL;
static ATOM wndclass = 0;
static HWND appwindow = NULL, conwindow = NULL;
static HICON appicon = NULL;
static HMENU appmenu = NULL;
static HANDLE outhandle = NULL;
static const int MAXLOGLINES = 200;
struct logline { int len; char buf[LOGSTRLEN]; };
static queue<logline, MAXLOGLINES> loglines;

static void cleanupsystemtray()
{
    NOTIFYICONDATA nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = appwindow;
    nid.uID = IDI_ICON1;
    Shell_NotifyIcon(NIM_DELETE, &nid);
}

static bool setupsystemtray(UINT uCallbackMessage)
{
	NOTIFYICONDATA nid;
	memset(&nid, 0, sizeof(nid));
	nid.cbSize = sizeof(nid);
	nid.hWnd = appwindow;
	nid.uID = IDI_ICON1;
	nid.uCallbackMessage = uCallbackMessage;
	nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	nid.hIcon = appicon;
	strcpy(nid.szTip, apptip);
	if(Shell_NotifyIcon(NIM_ADD, &nid) != TRUE)
        return false;
    atexit(cleanupsystemtray);
    return true;
}

#if 0
static bool modifysystemtray()
{
	NOTIFYICONDATA nid;
	memset(&nid, 0, sizeof(nid));
	nid.cbSize = sizeof(nid);
	nid.hWnd = appwindow;
	nid.uID = IDI_ICON1;
	nid.uFlags = NIF_TIP;
	strcpy(nid.szTip, apptip);
	return Shell_NotifyIcon(NIM_MODIFY, &nid) == TRUE;
}
#endif

static void cleanupwindow()
{
	if(!appinstance) return;
	if(appmenu)
	{
		DestroyMenu(appmenu);
		appmenu = NULL;
	}
	if(wndclass)
	{
		UnregisterClass(MAKEINTATOM(wndclass), appinstance);
		wndclass = 0;
	}
}

static BOOL WINAPI consolehandler(DWORD dwCtrlType)
{
    switch(dwCtrlType)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            exit(EXIT_SUCCESS);
            return TRUE;
    }
    return FALSE;
}

static void writeline(logline &line)
{
    static uchar ubuf[512];
    size_t len = strlen(line.buf), carry = 0;
    while(carry < len)
    {
        size_t numu = encodeutf8(ubuf, sizeof(ubuf), &((uchar *)line.buf)[carry], len - carry, &carry);
        DWORD written = 0;
        WriteConsole(outhandle, ubuf, numu, &written, NULL);
    }     
}

static void setupconsole()
{
	if(conwindow) return;
    if(!AllocConsole()) return;
	SetConsoleCtrlHandler(consolehandler, TRUE);
	conwindow = GetConsoleWindow();
    SetConsoleTitle(apptip);
	//SendMessage(conwindow, WM_SETICON, ICON_SMALL, (LPARAM)appicon);
	SendMessage(conwindow, WM_SETICON, ICON_BIG, (LPARAM)appicon);
    outhandle = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO coninfo;
    GetConsoleScreenBufferInfo(outhandle, &coninfo);
    coninfo.dwSize.Y = MAXLOGLINES;
    SetConsoleScreenBufferSize(outhandle, coninfo.dwSize);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    loopv(loglines) writeline(loglines[i]);
}

enum
{
	MENU_OPENCONSOLE = 0,
	MENU_SHOWCONSOLE,
	MENU_HIDECONSOLE,
	MENU_EXIT
};

static LRESULT CALLBACK handlemessages(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
		case WM_APP:
			SetForegroundWindow(hWnd);
			switch(lParam)
			{
				//case WM_MOUSEMOVE:
				//	break;
				case WM_LBUTTONUP:
				case WM_RBUTTONUP:
				{
					POINT pos;
					GetCursorPos(&pos);
					TrackPopupMenu(appmenu, TPM_CENTERALIGN|TPM_BOTTOMALIGN|TPM_RIGHTBUTTON, pos.x, pos.y, 0, hWnd, NULL);
					PostMessage(hWnd, WM_NULL, 0, 0);
					break;
				}
			}
			return 0;
		case WM_COMMAND:
			switch(LOWORD(wParam))
			{
                case MENU_OPENCONSOLE:
					setupconsole();
					if(conwindow) ModifyMenu(appmenu, 0, MF_BYPOSITION|MF_STRING, MENU_HIDECONSOLE, "Hide Console");
                    break;
				case MENU_SHOWCONSOLE:
					ShowWindow(conwindow, SW_SHOWNORMAL);
					ModifyMenu(appmenu, 0, MF_BYPOSITION|MF_STRING, MENU_HIDECONSOLE, "Hide Console"); 
					break;
				case MENU_HIDECONSOLE:
					ShowWindow(conwindow, SW_HIDE);
					ModifyMenu(appmenu, 0, MF_BYPOSITION|MF_STRING, MENU_SHOWCONSOLE, "Show Console");
					break;
				case MENU_EXIT:
					PostMessage(hWnd, WM_CLOSE, 0, 0);
					break;
			}
			return 0;
		case WM_CLOSE:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static void setupwindow(const char *title)
{
    copystring(apptip, title);
    //appinstance = GetModuleHandle(NULL);
    if(!appinstance) fatal("failed getting application instance");
    appicon = LoadIcon(appinstance, MAKEINTRESOURCE(IDI_ICON1));//(HICON)LoadImage(appinstance, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    if(!appicon) spdlog::get("global")->error("failed loading icon");

    appmenu = CreatePopupMenu();
    if(!appmenu) fatal("failed creating popup menu");
    AppendMenu(appmenu, MF_STRING, MENU_OPENCONSOLE, "Open Console");
    AppendMenu(appmenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(appmenu, MF_STRING, MENU_EXIT, "Exit");
    //SetMenuDefaultItem(appmenu, 0, FALSE);

    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.hCursor = NULL; //LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = appicon;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = title;
    wc.style = 0;
    wc.hInstance = appinstance;
    wc.lpfnWndProc = handlemessages;
    wc.cbWndExtra = 0;
    wc.cbClsExtra = 0;
    wndclass = RegisterClass(&wc);
    if(!wndclass) fatal("failed registering window class");
	
    appwindow = CreateWindow(MAKEINTATOM(wndclass), title, 0, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, HWND_MESSAGE, NULL, appinstance, NULL);
    if(!appwindow) fatal("failed creating window");

    atexit(cleanupwindow);

    if(!setupsystemtray(WM_APP)) fatal("failed adding to system tray");
}

static char *parsecommandline(const char *src, vector<char *> &args)
{
    char *buf = new char[strlen(src) + 1], *dst = buf;
    for(;;)
    {
        while(isspace(*src)) src++;
        if(!*src) break;
        args.add(dst);
		for(bool quoted = false; *src && (quoted || !isspace(*src)); src++)
        {
            if(*src != '"') *dst++ = *src;
			else if(dst > buf && src[-1] == '\\') dst[-1] = '"';
			else quoted = !quoted;
		}
		*dst++ = '\0';
    }
    args.add(NULL);
    return buf;
}
                

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw)
{
    vector<char *> args;
    char *buf = parsecommandline(GetCommandLine(), args);
	appinstance = hInst;
#ifdef STANDALONE
    int standalonemain(int argc, char **argv);
    int status = standalonemain(args.length()-1, args.getbuf());
    #undef main
    #define main standalonemain
#else
    SDL_SetMainReady();
    int status = SDL_main(args.length()-1, args.getbuf());
#endif
    delete[] buf;
    exit(status);
    return 0;
}

#endif

static bool dedicatedserver = false;

bool isdedicatedserver() { return dedicatedserver; }

void rundedicatedserver()
{
    dedicatedserver = true;
    spdlog::get("global")->info("dedicated server started, waiting for clients...");
#ifdef WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	for(;;)
	{
		MSG msg;
		while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if(msg.message == WM_QUIT) exit(EXIT_SUCCESS);
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		serverslice(true, 5);
	}
#else
    for(;;) serverslice(true, 5);
#endif
    dedicatedserver = false;
}

bool servererror(bool dedicated, const char *desc)
{
#ifndef STANDALONE
    if(!dedicated)
    {
        spdlog::get("global")->error(desc);
        cleanupserver();
    }
    else
#endif
        fatal("%s", desc);
    return false;
}
  
bool setuplistenserver(bool dedicated)
{
    ENetAddress address = { ENET_HOST_ANY, enet_uint16(serverport <= 0 ? server_port() : serverport) };
    if(*serverip)
    {
        if(enet_address_set_host(&address, serverip)<0) spdlog::get("global")->warn("WARNING: server ip not resolved");
        else serveraddress.host = address.host;
    }
    serverhost = enet_host_create(&address, min(maxclients + server::reserveclients(), MAXCLIENTS), NUM_ENET_CHANNELS, 0, serveruprate);
    if(!serverhost) return servererror(dedicated, "could not create server host");
    serverhost->duplicatePeers = maxdupclients ? maxdupclients : MAXCLIENTS;
    address.port = server_info_port(serverport > 0 ? serverport : -1);
    pongsock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if(pongsock != ENET_SOCKET_NULL && enet_socket_bind(pongsock, &address) < 0)
    {
        enet_socket_destroy(pongsock);
        pongsock = ENET_SOCKET_NULL;
    }
    if(pongsock == ENET_SOCKET_NULL) return servererror(dedicated, "could not create server info socket");
    else enet_socket_set_option(pongsock, ENET_SOCKOPT_NONBLOCK, 1);
    address.port = lan_info_port();
    lansock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if(lansock != ENET_SOCKET_NULL && (enet_socket_set_option(lansock, ENET_SOCKOPT_REUSEADDR, 1) < 0 || enet_socket_bind(lansock, &address) < 0))
    {
        enet_socket_destroy(lansock);
        lansock = ENET_SOCKET_NULL;
    }
    if(lansock == ENET_SOCKET_NULL) spdlog::get("global")->warn("WARNING: could not create LAN server info socket");
    else enet_socket_set_option(lansock, ENET_SOCKOPT_NONBLOCK, 1);
    return true;
}

void initserver(bool listen, bool dedicated)
{
    if(dedicated)
    {
#ifdef WIN32
        setupwindow("Inexor server");
#endif
    }
    
    server::serverinit();

    if(initscript) execfile(initscript);
    else execfile("server-init.cfg", false);

    if(listen) setuplistenserver(dedicated);

    if(listen)
    {
        dedicatedserver = dedicated;
        if(dedicated) rundedicatedserver(); // never returns
#ifndef STANDALONE
        else spdlog::get("global")->info("listen server started");
#endif
    }
}

#ifndef STANDALONE
void startlistenserver(int *usemaster)
{
    if(serverhost) { spdlog::get("global")->error("listen server is already running"); return; }

    if(!setuplistenserver(false)) return;
    spdlog::get("global")->info("listen server started for {0} clients", *maxclients);
}
COMMAND(startlistenserver, "i");

void stoplistenserver()
{
    if(!serverhost) { spdlog::get("global")->error("listen server is not running"); return; }

    kicknonlocalclients();
    enet_host_flush(serverhost);
    cleanupserver();

    spdlog::get("global")->info("listen server stopped");
}
COMMAND(stoplistenserver, "");
#endif

bool serveroption(const char *opt)
{
    switch(opt[1])
    {
        case 'u': setvar("serveruprate", atoi(opt+2)); return true;
        case 'c': setvar("maxclients", atoi(opt+2)); return true;
        case 'i': setsvar("serverip", opt+2); return true;
        case 'j': setvar("serverport", atoi(opt+2)); return true;
#ifdef STANDALONE
        case 'q': spdlog::get("global")->debug("Using home directory: {}", opt); sethomedir(opt+2); return true;
        case 'k': spdlog::get("global")->debug("Adding package directory: {}", opt); addpackagedir(opt+2); return true;
        case 'x': spdlog::get("global")->debug("Setting server init script: {}", opt); initscript = opt+2; return true;
#endif
        default: return false;
    }
}

vector<const char *> gameargs;

#ifdef STANDALONE

void parseoptions(vector<const char *> &args)
{
    loopv(args)
        if(!serveroption(args[i]))
            spdlog::get("global")->error("unknown command-line option: {0}", args[i]);
}

inexor::util::Logging logging;

int main(int argc, char **argv)
{
    logging.initDefaultLoggers();
    UNUSED inexor::crashreporter::CrashReporter SingletonStackwalker; // We only need to initialse it, not use it.
    if(enet_initialize()<0) fatal("Unable to initialise network module");
    atexit(enet_deinitialize);
    enet_time_set(0);
    for(int i = 1; i<argc; i++) if(argv[i][0]!='-' || !serveroption(argv[i])) gameargs.add(argv[i]);
    parseoptions(gameargs);
    initserver(true, true);
    return EXIT_SUCCESS;
}
#endif
