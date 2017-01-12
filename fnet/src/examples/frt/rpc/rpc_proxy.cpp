// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#include <vespa/fastos/fastos.h>
#include <vespa/log/log.h>
LOG_SETUP("rpc_proxy");
#include <vespa/fnet/frt/frt.h>

//-----------------------------------------------------------------------------

struct Session
{
    FNET_Connection *client;
    FRT_Target      *server;
    uint32_t         id;
    uint32_t         finiCnt;

    Session(uint32_t xid) : client(NULL), server(NULL), id(xid), finiCnt(0) {}
    ~Session() { assert(client == NULL && server == NULL && finiCnt == 2); }

private:
    Session(const Session &);
    Session &operator=(const Session &);
};

//-----------------------------------------------------------------------------

class RPCProxy : public FRT_Invokable
{
private:
    FRT_Supervisor &_supervisor;
    const char     *_spec;
    bool            _verbose;
    uint32_t        _currID;
    char            _prefixStr[256];

    RPCProxy(const RPCProxy &);
    RPCProxy &operator=(const RPCProxy &);

public:
    RPCProxy(FRT_Supervisor &supervisor,
             const char *spec,
             bool verbose)
        : _supervisor(supervisor),
          _spec(spec),
          _verbose(verbose),
          _currID(0),
          _prefixStr() {}

    bool IsVerbose() const { return _verbose; }
    const char *GetPrefix(FRT_RPCRequest *req);
    void PrintMethod(FRT_RPCRequest *req, const char *desc);
    void Done(FRT_RPCRequest *req);
    void HOOK_Mismatch(FRT_RPCRequest *req);
    void HOOK_Init(FRT_RPCRequest *req);
    void HOOK_Down(FRT_RPCRequest *req);
    void HOOK_Fini(FRT_RPCRequest *req);
    static Session *GetSession(FRT_RPCRequest *req)
    {
        return (Session *) req->GetConnection()->GetContext()._value.VOIDP;
    }
};

//-----------------------------------------------------------------------------

class ReqDone : public FRT_IRequestWait
{
private:
    RPCProxy &_proxy;

public:
    ReqDone(RPCProxy &proxy) : _proxy(proxy) {}
    virtual void RequestDone(FRT_RPCRequest *req);
};

void
ReqDone::RequestDone(FRT_RPCRequest *req)
{
    _proxy.Done(req);
}

//-----------------------------------------------------------------------------

const char *
RPCProxy::GetPrefix(FRT_RPCRequest *req)
{
    FastOS_Time t;
    tm currTime;
    tm *currTimePt;

    t.SetNow();
    time_t secs = t.Secs();
    currTimePt = localtime_r(&secs, &currTime);
    assert(currTimePt == &currTime);
    (void) currTimePt;

    char rid[32];
    if (req->GetContext()._value.CHANNEL != NULL) {
        sprintf(rid, "[rid=%u]", req->GetContext()._value.CHANNEL->GetID());
    } else {
        rid[0] = '\0';
    }

    sprintf(_prefixStr, "[%04d-%02d-%02d %02d:%02d:%02d:%03d][sid=%u]%s",
            currTime.tm_year + 1900,
            currTime.tm_mon + 1,
            currTime.tm_mday,
            currTime.tm_hour,
            currTime.tm_min,
            currTime.tm_sec,
            (int)(t.GetMicroSeconds() / 1000),
            GetSession(req)->id,
            rid);

    return _prefixStr;
}


void
RPCProxy::PrintMethod(FRT_RPCRequest *req, const char *desc)
{
    fprintf(stdout, "%s %s: %s\n", GetPrefix(req), desc,
            req->GetMethodName());
}


void
RPCProxy::Done(FRT_RPCRequest *req)
{
    PrintMethod(req, "RETURN");
    if (IsVerbose()) {
        req->GetReturn()->Print(8);
    }
    req->Return();
}


void
RPCProxy::HOOK_Mismatch(FRT_RPCRequest *req)
{
    PrintMethod(req, "INVOKE");
    if (IsVerbose()) {
        req->GetParams()->Print(8);
    }
    req->Detach();
    req->SetError(FRTE_NO_ERROR, "");
    if (req->GetConnection()->IsServer() &&
        GetSession(req)->server != NULL)
    {
        GetSession(req)->server->InvokeAsync(req, 60.0,
                                             new (req->getStash())
                                             ReqDone(*this));
    } else if (req->GetConnection()->IsClient() &&
               GetSession(req)->client != NULL)
    {
        FRT_Supervisor::InvokeAsync(GetSession(req)->client->Owner(),
                                    GetSession(req)->client,
                                    req, 60.0,
                                    new (req->getStash())
                                    ReqDone(*this));
    } else {
        req->SetError(FRTE_RPC_CONNECTION);
        req->Return();
    }
}


void
RPCProxy::HOOK_Init(FRT_RPCRequest *req)
{
    if (req->GetConnection()->IsClient()) {
        return;
    }
    Session *session = new Session(_currID++);
    session->client = req->GetConnection();
    session->server =
        _supervisor.Get2WayTarget(_spec,
                                  FNET_Context((void *) session));
    session->client->SetContext(FNET_Context((void *) session));
    if (session->server->GetConnection() == NULL ||
        session->server->GetConnection()->GetState()
        > FNET_Connection::FNET_CONNECTED)
    {
        session->finiCnt = 1;
        session->client->Owner()->Close(session->client);
    }
    fprintf(stdout, "%s INIT\n", GetPrefix(req));
}


void
RPCProxy::HOOK_Down(FRT_RPCRequest *req)
{
    Session *session = GetSession(req);
    if (req->GetConnection()->IsClient()) {
        if (session->client != NULL) {
            session->client->Owner()->Close(session->client);
        }
    } else {
        session->server->SubRef();
        session->client = NULL;
        session->server = NULL;
    }
}


void
RPCProxy::HOOK_Fini(FRT_RPCRequest *req)
{
    if (++GetSession(req)->finiCnt == 2) {
        fprintf(stdout, "%s FINI\n", GetPrefix(req));
        delete GetSession(req);
    }
}

//-----------------------------------------------------------------------------

class App : public FastOS_Application
{
public:
    virtual int Main();
};

int
App::Main()
{
    FNET_SignalShutDown::hookSignals();
    // would like to turn off FNET logging somehow
    if (_argc < 3) {
        fprintf(stderr, "usage: %s <listenspec> <connectspec> [verbose]\n", _argv[0]);
        return 1;
    }
    bool verbose = (_argc > 3) && (strcmp(_argv[3], "verbose") == 0);

    FRT_Supervisor supervisor;
    RPCProxy       proxy(supervisor, _argv[2], verbose);

    supervisor.GetReflectionManager()->Reset();
    supervisor.SetSessionInitHook(FRT_METHOD(RPCProxy::HOOK_Init), &proxy);
    supervisor.SetSessionDownHook(FRT_METHOD(RPCProxy::HOOK_Down), &proxy);
    supervisor.SetSessionFiniHook(FRT_METHOD(RPCProxy::HOOK_Fini), &proxy);
    supervisor.SetMethodMismatchHook(FRT_METHOD(RPCProxy::HOOK_Mismatch),
                                     &proxy);
    supervisor.Listen(_argv[1]);
    FNET_SignalShutDown ssd(*supervisor.GetTransport());
    supervisor.Main();
    return 0;
}


int
main(int argc, char **argv)
{
    App app;
    return app.Entry(argc, argv);
}
