#ifndef OSC_LAN_H
#define OSC_LAN_H

// Plain blocking OS sockets instead of QTcpSocket: the original application
// invokes its LAN transport from several different QThreads. QTcpSocket is
// thread-affine and must not be used that way.
class osclan
{
public:
    osclan();
    ~osclan();
    int open(const char *host_or_ip);
    void close();
    int write(const char *data, int len);
    int read(char *data, int len);   // one chunk, kept for compatibility
    int readExact(char *data, int len);
    int readLine(char *data, int max_len); // includes '\n'; -1 on failure
    void flush();

private:
    struct Impl;
    Impl *p;
    osclan(const osclan &) = delete;
    osclan &operator=(const osclan &) = delete;
};

#ifdef __cplusplus
extern "C" {
#endif
struct tmcdev;
struct tmcdev * tmc_open_lan(const char *host_or_ip);
void tmc_close_lan(struct tmcdev *dev); // preserved from the original header
#ifdef __cplusplus
}
#endif

#endif // OSC_LAN_H
