#include "../../src/guest_control.cpp"
#include <cassert>
#include <iostream>
#include <utility>
#include <tuple>
static double ticks = 0;
static std::vector<std::tuple<double,PIC_EventHandler,Bitu>> queue;
static std::vector<std::pair<KBD_KEYS,bool>> keys;
static std::vector<std::pair<int,bool>> buttons;
static std::vector<std::pair<int,int>> moves;
double PIC_FullIndex() { return ticks; }
void PIC_AddEvent(PIC_EventHandler f,double delay,Bitu id) { queue.emplace_back(ticks+delay,f,id); }
void PIC_RemoveEvents(PIC_EventHandler f) {
    queue.erase(std::remove_if(queue.begin(),queue.end(),[f](const auto &e){return std::get<1>(e)==f;}),queue.end());
}
void KEYBOARD_AddKey(KBD_KEYS key,bool pressed) { keys.emplace_back(key,pressed); }
void Mouse_ButtonPressed(uint8_t b) { buttons.emplace_back(b,true); }
void Mouse_ButtonReleased(uint8_t b) { buttons.emplace_back(b,false); }
void Mouse_CursorInjected(float x,float y,float,float,bool relative) { assert(relative); moves.emplace_back(int(x),int(y)); }
bool mem_readb_checked(LinearPt address,uint8_t *v) { *v=uint8_t(address); return address == 0xdead; }
static void advance(double target) {
    for (;;) {
        std::stable_sort(queue.begin(),queue.end(),[](const auto &a,const auto &b){return std::get<0>(a)<std::get<0>(b);});
        if (queue.empty() || std::get<0>(queue.front())>target) break;
        const auto e=queue.front(); queue.erase(queue.begin()); ticks=std::get<0>(e); std::get<1>(e)(std::get<2>(e));
    }
    ticks=target;
}
static int connect_client(const std::string &path) {
    int fd=socket(AF_UNIX,SOCK_STREAM,0); assert(fd>=0);
    sockaddr_un addr={}; addr.sun_family=AF_UNIX; strcpy(addr.sun_path,path.c_str());
    assert(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0); assert(nonblocking(fd));
    GuestControl_Poll(); return fd;
}
static std::string drain(int fd) {
    std::string result; char buffer[8192]; ssize_t n;
    while ((n=recv(fd,buffer,sizeof(buffer),0))>0) result.append(buffer,size_t(n));
    return result;
}
static void send_text(int fd,const std::string &text) {
    assert(send(fd,text.data(),text.size(),0)==ssize_t(text.size()));
    for (size_t i=0;i<text.size()/256+2;++i) GuestControl_Poll();
}
int main(int argc,char **argv) {
    assert(argc==2);
    const std::vector<std::string> invalid={"KEY -1 10 a","KEY 0 -1 a","KEY 0 0 a","KEY 600000 1 a",
        "KEY 9223372036854775807 9223372036854775807 a","KEY 0 10 bad","KEY 0 10 a extra",
        "MOUSE MOVE -1 0 0","MOUSE MOVE 0 -32769 0","MOUSE BUTTON 0 left invalid",
        "MEMDUMP 0 -1 1 out","MEMDUMP 0 0 1 out extra","MEMDUMP 0 ffffff 2 out",
        "MEMDUMP 0 0 1 ../out","MEMDUMP 0 0 1 /out",std::string("KEY 0 10 a\0",11)};
    for (const auto &line:invalid) { Event e; assert(!parse(line,e)); }
    const KBD_KEYS expected[]={KBD_a,KBD_b,KBD_c,KBD_d,KBD_e,KBD_f,KBD_g,KBD_h,KBD_i,KBD_j,KBD_k,KBD_l,KBD_m,KBD_n,KBD_o,KBD_p,KBD_q,KBD_r,KBD_s,KBD_t,KBD_u,KBD_v,KBD_w,KBD_x,KBD_y,KBD_z};
    for (int i=0;i<26;++i) {Event e; assert(parse(std::string("KEY 0 10 ")+char('a'+i),e));assert(e.key==expected[i]);}
    Event e; assert(parse("MOUSE MOVE 0 -10 12",e)&&e.x==-10&&e.y==12);
    // Disabled mode has no descriptors, socket, log, or scheduled events.
    unsetenv("DOSBOX_X_GUEST_CONTROL_SOCKET"); unsetenv("DOSBOX_X_GUEST_CONTROL_DIR");
    GuestControl_Poll(); assert(s.disabled && s.listener<0 && s.log<0 && queue.empty());
    s=State{};
    const std::string dir=argv[1], path=dir+"/control.sock";
    assert(setenv("DOSBOX_X_GUEST_CONTROL_SOCKET",path.c_str(),1)==0);
    assert(setenv("DOSBOX_X_GUEST_CONTROL_DIR",dir.c_str(),1)==0);
    GuestControl_Poll(); assert(!s.disabled);
    struct stat st; assert(stat(path.c_str(),&st)==0 && (st.st_mode&0777)==0600);
    int fd=connect_client(path);
    send_text(fd,"KEY 0 10 b\n"); assert(s.events.size()==1);
    advance(0); assert(keys.back()==std::make_pair(KBD_b,true));
    advance(10); assert(keys.back()==std::make_pair(KBD_b,false)); assert(s.events.empty());
    GuestControl_Poll(); assert(drain(fd).find("done")!=std::string::npos);
    send_text(fd,"MOUSE BUTTON 0 right press\nMOUSE BUTTON 1 right release\nMOUSE MOVE 0 -10 12\n");
    advance(11); assert(buttons[0]==std::make_pair(1,true)&&buttons[1]==std::make_pair(1,false)); assert(moves.back()==std::make_pair(-10,12));
    const auto before=moves.size();
    send_text(fd,std::string(600,'x')); send_text(fd,"MOUSE MOVE 0 1 1\n"); advance(12); assert(moves.size()==before);
    send_text(fd,"MOUSE MOVE 0 2 3\n"); advance(12); assert(moves.back()==std::make_pair(2,3));
    send_text(fd,"MEMDUMP 0 10 4 bytes.bin\n"); advance(12);
    int dumpfd=open((dir+"/bytes.bin").c_str(),O_RDONLY); char data[4]; assert(read(dumpfd,data,4)==4);close(dumpfd);assert(data[0]==16&&data[3]==19);
    send_text(fd,"MEMDUMP 0 20 4 bytes.bin\nMEMDUMP 0 dead 1 bad.bin\n");advance(12);
    dumpfd=open((dir+"/bytes.bin").c_str(),O_RDONLY);assert(read(dumpfd,data,4)==4&&data[0]==16);close(dumpfd);assert(access((dir+"/bad.bin").c_str(),F_OK)!=0);
    send_text(fd,"MOUSE MOVE 50 7 8\n"); const auto old_session=s.session; close(fd);GuestControl_Poll();
    fd=connect_client(path); assert(s.session!=old_session); advance(62); GuestControl_Poll();assert(drain(fd).empty());assert(moves.back()==std::make_pair(7,8));
    // Queue cap rejects additional work without dropping accepted events.
    drain(fd);
    for (unsigned i=0;i<MAX_PENDING;++i) schedule("MOUSE MOVE 1000 0 0");
    assert(s.events.size()==MAX_PENDING);
    schedule("MOUSE MOVE 1000 1 1"); assert(s.events.size()==MAX_PENDING);
    advance(1062); assert(s.events.empty());
    GuestControl_Poll(); drain(fd);
    // Output backpressure disconnects rather than retaining unbounded replies.
    for (unsigned i=0;i<2000;++i) schedule("invalid");
    assert(s.client<0 && s.output_bytes==0 && s.output.empty());
    close(fd); fd=connect_client(path); assert(drain(fd).empty());
    send_text(fd,"KEY 0 100 a\nMOUSE BUTTON 0 middle press\n"); advance(1062);assert(!s.pressed_keys.empty());
    s.next=uint64_t(std::numeric_limits<Bitu>::max());
    const size_t pending_before=s.events.size();
    schedule("MOUSE MOVE 0 3 4"); assert(s.failed && s.events.size()==pending_before);
    GuestControl_Poll();assert(queue.empty()&&s.events.empty());assert(keys.back()==std::make_pair(KBD_a,false));assert(buttons.back()==std::make_pair(2,false));assert(access(path.c_str(),F_OK)!=0);close(fd);
    std::cout<<"PASS: actual C++ parser, alphabet, callbacks, socket framing, dump integrity, reconnect isolation, shutdown\n";
}
