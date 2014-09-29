//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/common/RippleSSLContext.h>
#include <ripple/http/Session.h>
#include <ripple/module/app/main/RPCHTTPServer.h>
#include <ripple/module/rpc/RPCHandler.h>
#include <ripple/module/rpc/RPCServerHandler.h>

namespace ripple {

class RPCHTTPServerImp
    : public RPCHTTPServer
    , public beast::LeakChecked <RPCHTTPServerImp>
    , public HTTP::Handler
{
public:
    Resource::Manager& m_resourceManager;
    beast::Journal m_journal;
    JobQueue& m_jobQueue;
    NetworkOPs& m_networkOPs;
    RPCServerHandler m_deprecatedHandler;
    HTTP::Server m_server;
    std::unique_ptr <RippleSSLContext> m_context;

    RPCHTTPServerImp (Stoppable& parent, JobQueue& jobQueue,
            NetworkOPs& networkOPs, Resource::Manager& resourceManager)
        : RPCHTTPServer (parent)
        , m_resourceManager (resourceManager)
        , m_journal (deprecatedLogs().journal("HTTP-RPC"))
        , m_jobQueue (jobQueue)
        , m_networkOPs (networkOPs)
        , m_deprecatedHandler (networkOPs, resourceManager)
        , m_server (*this, deprecatedLogs().journal("HTTP"))
    {
        if (getConfig ().RPC_SECURE == 0)
        {
            m_context.reset (RippleSSLContext::createBare ());
        }
        else
        {
            m_context.reset (RippleSSLContext::createAuthenticated (
                getConfig ().RPC_SSL_KEY,
                    getConfig ().RPC_SSL_CERT,
                        getConfig ().RPC_SSL_CHAIN));
        }
    }

    ~RPCHTTPServerImp()
    {
        m_server.stop();
    }

    void
    setup (beast::Journal journal) override
    {
        if (! getConfig ().getRpcIP().empty () &&
              getConfig ().getRpcPort() != 0)
        {
            beast::IP::Endpoint ep (beast::IP::Endpoint::from_string (getConfig().getRpcIP()));

            // VFALCO TODO IP address should not have an "unspecified" state
            //if (! is_unspecified (ep))
            {
                HTTP::Port port;
                port.security = HTTP::Port::Security::allow_ssl;
                port.addr = ep.at_port(0);
                if (getConfig ().getRpcPort() != 0)
                    port.port = getConfig ().getRpcPort();
                else
                    port.port = ep.port();
                port.context = m_context.get ();

                HTTP::Ports ports;
                ports.push_back (port);
                m_server.setPorts (ports);
            }
        }
        else
        {
            journal.info << "RPC interface: disabled";
        }
    }

    //--------------------------------------------------------------------------
    //
    // Stoppable
    //

    void
    onStop() override
    {
        m_server.stopAsync();
    }

    void
    onChildrenStopped() override
    {
    }

    //--------------------------------------------------------------------------
    //
    // HTTP::Handler
    //

    void
    onAccept (HTTP::Session& session) override
    {
        // Reject non-loopback connections if RPC_ALLOW_REMOTE is not set
        if (! getConfig().RPC_ALLOW_REMOTE &&
            ! beast::IP::is_loopback (session.remoteAddress()))
        {
            session.close (false);
        }
    }

    void
    onRequest (HTTP::Session& session) override
    {
        // Check user/password authorization
        auto const headers (build_map (session.message().headers));
        if (! HTTPAuthorized (headers))
        {
            session.write (HTTPReply (403, "Forbidden"));
            session.close (true);
            return;
        }

#if 0
        // Synchronous version that doesn't use job queue
        Job job;
        processSession (job, session);

#else
        session.detach();

        m_jobQueue.addJob (jtCLIENT, "RPC-Client", std::bind (
            &RPCHTTPServerImp::processSession, this, std::placeholders::_1,
                std::ref (session)));
#endif
    }

    void
    onClose (HTTP::Session& session,
        boost::system::error_code const&) override
    {
    }

    void
    onStopped (HTTP::Server&) override
    {
        stopped();
    }

    //--------------------------------------------------------------------------

    // Dispatched on the job queue
    void processSession (Job& job, HTTP::Session& session)
    {
#if 0
        // Goes through the old code
        session.write (m_deprecatedHandler.processRequest (
            session.content(), session.remoteAddress().at_port(0)));
#else
        auto const s (to_string(session.message().body));
        session.write (processRequest (to_string(session.message().body),
            session.remoteAddress().at_port(0)));
#endif

        if (session.message().keep_alive())
        {
            session.complete();
        }
        else
        {
            session.close (true);
        }
    }

    std::string createResponse (
        int statusCode,
        std::string const& description)
    {
        return HTTPReply (statusCode, description);
    }

    // Stolen directly from RPCServerHandler
    std::string
    processRequest (std::string const& request,
        beast::IP::Endpoint const& remoteIPAddress)
    {
        Json::Value jvRequest;
        {
            Json::Reader reader;

            if ((request.size () > 1000000) ||
                ! reader.parse (request, jvRequest) ||
                jvRequest.isNull () ||
                ! jvRequest.isObject ())
            {
                return createResponse (400, "Unable to parse request");
            }
        }

        Config::Role const role (getConfig ().getAdminRole (jvRequest, remoteIPAddress));

        Resource::Consumer usage;

        if (role == Config::ADMIN)
            usage = m_resourceManager.newAdminEndpoint (remoteIPAddress.to_string());
        else
            usage = m_resourceManager.newInboundEndpoint(remoteIPAddress);

        if (usage.disconnect ())
            return createResponse (503, "Server is overloaded");

        // Parse id now so errors from here on will have the id
        //
        // VFALCO NOTE Except that "id" isn't included in the following errors...
        //
        Json::Value const id = jvRequest ["id"];

        Json::Value const method = jvRequest ["method"];

        if (method.isNull ())
        {
            return createResponse (400, "Null method");
        }
        else if (! method.isString ())
        {
            return createResponse (400, "method is not string");
        }

        std::string strMethod = method.asString ();

        // Parse params
        Json::Value params = jvRequest ["params"];

        if (params.isNull ())
        {
            params = Json::Value (Json::arrayValue);
        }
        else if (!params.isArray ())
        {
            return HTTPReply (400, "params unparseable");
        }

        // VFALCO TODO Shouldn't we handle this earlier?
        //
        if (role == Config::FORBID)
        {
            // VFALCO TODO Needs implementing
            // FIXME Needs implementing
            // XXX This needs rate limiting to prevent brute forcing password.
            return HTTPReply (403, "Forbidden");
        }

        std::string response;

        m_journal.debug << "Query: " << strMethod << params;

        RPCHandler rpcHandler (m_networkOPs);

        Resource::Charge loadType = Resource::feeReferenceRPC;

        Json::Value const result (rpcHandler.doRpcCommand (
            strMethod, params, role, loadType));

        usage.charge (loadType);

        m_journal.debug << "Reply: " << result;

        response = JSONRPCReply (result, Json::Value (), id);

        return createResponse (200, response);
    }

    //
    // PropertyStream
    //

    void
    onWrite (beast::PropertyStream::Map& map) override
    {
        m_server.onWrite (map);
    }
};

//------------------------------------------------------------------------------

RPCHTTPServer::RPCHTTPServer (Stoppable& parent)
    : Stoppable ("RPCHTTPServer", parent)
    , Source ("http")
{
}

//------------------------------------------------------------------------------

std::unique_ptr <RPCHTTPServer>
make_RPCHTTPServer (beast::Stoppable& parent, JobQueue& jobQueue,
    NetworkOPs& networkOPs, Resource::Manager& resourceManager)
{
    return std::make_unique <RPCHTTPServerImp> (
        parent, jobQueue, networkOPs, resourceManager);
}

}