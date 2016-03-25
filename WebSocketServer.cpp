/** --------------------------------------------------------------------------
 *  WebSocketServer.cpp
 *
 *  Base class that WebSocket implementations must inherit from.  Handles the
 *  client connections and calls the child class callbacks for connection
 *  events like onConnect, onMessage, and onDisconnect.
 *
 *  Author    : Jason Kruse <jason@jasonkruse.com> or @mnisjk
 *  Copyright : 2014
 *  License   : BSD (see LICENSE)
 *  -------------------------------------------------------------------------- 
 **/

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>
#include "libwebsockets.h"
#include "Util.h"
#include "WebSocketServer.h"

using namespace std;

// 0 for unlimited
#define MAX_BUFFER_SIZE 0

const char* resource_path = "client";

// Nasty hack because certain callbacks are statically defined
WebSocketServer *self;

struct per_session_data__http {
    lws_filefd_type fd;
};


static int callback_http(   struct lws *wsi, 
                            enum lws_callback_reasons reason, 
                            void *user, 
                            void *in, 
                            size_t len );

static int callback_main(   struct lws *wsi, 
                            enum lws_callback_reasons reason, 
                            void *user, 
                            void *in, 
                            size_t len )
{
    int fd;
    unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 512 + LWS_SEND_BUFFER_POST_PADDING];
    unsigned char *p = &buf[LWS_SEND_BUFFER_PRE_PADDING];
    
    switch( reason ) {
        case LWS_CALLBACK_ESTABLISHED:
            self->onConnectWrapper( lws_get_socket_fd( wsi ) );
            lws_callback_on_writable( wsi );
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            fd = lws_get_socket_fd( wsi );
            while( !self->connections[fd]->buffer.empty( ) )
            {
                string message = self->connections[fd]->buffer.front( ); 
                int charsSent = lws_write( wsi, (unsigned char*)message.c_str( ), message.length( ), LWS_WRITE_TEXT );
                if( charsSent < message.length( ) ) 
                    self->onErrorWrapper( fd, string( "Error writing to socket" ) );
                else
                    // Only pop the message if it was sent successfully.
                    self->connections[fd]->buffer.pop_front( ); 
            }
            lws_callback_on_writable( wsi );
            break;
        
        case LWS_CALLBACK_RECEIVE:
            self->onMessage( lws_get_socket_fd( wsi ), string( (const char *)in ) );
            break;

        case LWS_CALLBACK_CLOSED:
            self->onDisconnectWrapper( lws_get_socket_fd( wsi ) );
            break;

        default:
            break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    {
        "http-only",        /* name */
        callback_http,      /* callback */
        sizeof (struct per_session_data__http), /* per_session_data_size */
        0,          /* max frame size / rx buffer */
    },
    {
        "dds",
        callback_main,
        0, // user data struct not used
        MAX_BUFFER_SIZE,
    },{ NULL, NULL, 0, 0 } // terminator
};

WebSocketServer::WebSocketServer( int port, const string certPath, const string& keyPath )
{
    this->_port     = port;
    this->_certPath = certPath;
    this->_keyPath  = keyPath; 

    lws_set_log_level( 0, lwsl_emit_syslog ); // We'll do our own logging, thank you.
    struct lws_context_creation_info info;
    memset( &info, 0, sizeof info );
    info.port = this->_port;
    info.iface = NULL;
    info.protocols = protocols;
#ifndef LWS_NO_EXTENSIONS
    info.extensions = lws_get_internal_extensions( );
#endif
    
    if( !this->_certPath.empty( ) && !this->_keyPath.empty( ) )
    {
        Util::log( "Using SSL certPath=" + this->_certPath + ". keyPath=" + this->_keyPath + "." );
        info.ssl_cert_filepath        = this->_certPath.c_str( );
        info.ssl_private_key_filepath = this->_keyPath.c_str( );
    } 
    else 
    {
        Util::log( "Not using SSL" );
        info.ssl_cert_filepath        = NULL;
        info.ssl_private_key_filepath = NULL;
    }
    info.gid = -1;
    info.uid = -1;
    info.options = 0;

    // keep alive
    info.ka_time = 60; // 60 seconds until connection is suspicious
    info.ka_probes = 10; // 10 probes after ^ time
    info.ka_interval = 10; // 10s interval for sending probes
    this->_context = lws_create_context( &info );
    if( !this->_context )
        throw "libwebsocket init failed";
    Util::log( "Server started on port " + Util::toString( this->_port ) ); 

    // Some of the libwebsocket stuff is define statically outside the class. This 
    // allows us to call instance variables from the outside.  Unfortunately this
    // means some attributes must be public that otherwise would be private. 
    self = this;
}

WebSocketServer::~WebSocketServer( )
{
    // Free up some memory
    for( map<int,Connection*>::const_iterator it = this->connections.begin( ); it != this->connections.end( ); ++it )
    {
        Connection* c = it->second;
        this->connections.erase( it->first );
        delete c;
    }
}

void WebSocketServer::onConnectWrapper( int socketID )
{
    Connection* c = new Connection;
    c->createTime = time( 0 );
    this->connections[ socketID ] = c;
    this->onConnect( socketID );
}

void WebSocketServer::onDisconnectWrapper( int socketID )
{
    this->onDisconnect( socketID );
    this->_removeConnection( socketID );
}

void WebSocketServer::onErrorWrapper( int socketID, const string& message )
{
    Util::log( "Error: " + message + " on socketID '" + Util::toString( socketID ) + "'" ); 
    this->onError( socketID, message );
    this->_removeConnection( socketID );
}

void WebSocketServer::send( int socketID, string data )
{
    // Push this onto the buffer. It will be written out when the socket is writable.
    this->connections[socketID]->buffer.push_back( data );
}

void WebSocketServer::broadcast( string data )
{
    for( map<int,Connection*>::const_iterator it = this->connections.begin( ); it != this->connections.end( ); ++it )
        this->send( it->first, data );
}

void WebSocketServer::setValue( int socketID, const string& name, const string& value )
{
    this->connections[socketID]->keyValueMap[name] = value;
}

string WebSocketServer::getValue( int socketID, const string& name )
{
    return this->connections[socketID]->keyValueMap[name];
}
int WebSocketServer::getNumberOfConnections( )
{
    return this->connections.size( );
}

void WebSocketServer::run( uint64_t timeout )
{
    while( 1 )
    {
        this->wait( timeout );
    }
}

void WebSocketServer::wait( uint64_t timeout )
{
    if( lws_service( this->_context, timeout ) < 0 )
        throw "Error polling for socket activity.";
}

void WebSocketServer::_removeConnection( int socketID )
{
    Connection* c = this->connections[ socketID ];
    this->connections.erase( socketID );
    delete c;
}


const char * get_mimetype(const char *file)
{
    int n = strlen(file);

    if (n < 5)
        return NULL;

    if (!strcmp(&file[n - 4], ".ico"))
        return "image/x-icon";

    if (!strcmp(&file[n - 4], ".png"))
        return "image/png";

    if (!strcmp(&file[n - 4], ".jpg"))
        return "image/jpg";

    if (!strcmp(&file[n - 4], ".gif"))
        return "image/gif";


    if (!strcmp(&file[n - 5], ".html"))
        return "text/html";

    if (!strcmp(&file[n - 4], ".css"))
        return "text/plain";

    if (!strcmp(&file[n - 3], ".js"))
        return "application/javascript";
    

    return "text/plain";
}


/* this protocol server (always the first one) handles HTTP,
 *
 * Some misc callbacks that aren't associated with a protocol also turn up only
 * here on the first protocol server.
 */

static int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user,
          void *in, size_t len)
{
    struct per_session_data__http *pss =
            (struct per_session_data__http *)user;
    unsigned char buffer[4096 + LWS_PRE];
    unsigned long amount, file_len, sent;
    char leaf_path[1024];
    const char *mimetype;
    char *other_headers;
    unsigned char *end;
    struct timeval tv;
    unsigned char *p;
    char buf[256];
    char b64[64];
    int n, m;

#ifdef EXTERNAL_POLL
    struct lws_pollargs *pa = (struct lws_pollargs *)in;
#endif

    switch (reason) {
    case LWS_CALLBACK_HTTP:
        {
            char name[100], rip[50];
            lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi), name,
                           sizeof(name), rip, sizeof(rip));
            sprintf(buf, "%s (%s)", name, rip);
            lwsl_notice("HTTP connect from %s\n", buf);
        }

        if (len < 1) {
            lws_return_http_status(wsi,
                        HTTP_STATUS_BAD_REQUEST, NULL);
            goto try_to_reuse;
        }

        /* this example server has no concept of directories */
        if (strchr((const char *)in + 1, '/')) {
            //lws_return_http_status(wsi, HTTP_STATUS_FORBIDDEN, NULL);
            //goto try_to_reuse;
        }

        /* if a legal POST URL, let it continue and accept data */
        if (lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI))
            return 0;

        /* check for the "send a big file by hand" example case */

        if (!strcmp((const char *)in, "/leaf.jpg")) {
            if (strlen(resource_path) > sizeof(leaf_path) - 10)
                return -1;
            sprintf(leaf_path, "%s/leaf.jpg", resource_path);

            /* well, let's demonstrate how to send the hard way */

            p = buffer + LWS_PRE;
            end = p + sizeof(buffer) - LWS_PRE;

            pss->fd = lws_plat_file_open(wsi, leaf_path, &file_len,
                             LWS_O_RDONLY);

            if (pss->fd == LWS_INVALID_FILE) {
                lwsl_err("faild to open file %s\n", leaf_path);
                return -1;
            }

            /*
             * we will send a big jpeg file, but it could be
             * anything.  Set the Content-Type: appropriately
             * so the browser knows what to do with it.
             *
             * Notice we use the APIs to build the header, which
             * will do the right thing for HTTP 1/1.1 and HTTP2
             * depending on what connection it happens to be working
             * on
             */
            if (lws_add_http_header_status(wsi, 200, &p, end))
                return 1;
            if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_SERVER,
                        (unsigned char *)"libwebsockets",
                    13, &p, end))
                return 1;
            if (lws_add_http_header_by_token(wsi,
                    WSI_TOKEN_HTTP_CONTENT_TYPE,
                        (unsigned char *)"image/jpeg",
                    10, &p, end))
                return 1;
            if (lws_add_http_header_content_length(wsi,
                                   file_len, &p,
                                   end))
                return 1;
            if (lws_finalize_http_header(wsi, &p, end))
                return 1;

            /*
             * send the http headers...
             * this won't block since it's the first payload sent
             * on the connection since it was established
             * (too small for partial)
             *
             * Notice they are sent using LWS_WRITE_HTTP_HEADERS
             * which also means you can't send body too in one step,
             * this is mandated by changes in HTTP2
             */

            *p = '\0';
            lwsl_info("%s\n", buffer + LWS_PRE);

            n = lws_write(wsi, buffer + LWS_PRE, p - (buffer + LWS_PRE),
                      LWS_WRITE_HTTP_HEADERS);
            if (n < 0) {
                lws_plat_file_close(wsi, pss->fd);
                return -1;
            }
            /*
             * book us a LWS_CALLBACK_HTTP_WRITEABLE callback
             */
            lws_callback_on_writable(wsi);
            break;
        }

        /* if not, send a file the easy way */
        strcpy(buf, resource_path);
        if (strcmp((const char *)in, "/")) {
            if (*((const char *)in) != '/')
                strcat(buf, "/");
            strncat(buf, (const char *)in, sizeof(buf) - strlen(resource_path));
        } else /* default file to serve */
            strcat(buf, "/index.html");
        buf[sizeof(buf) - 1] = '\0';

        /* refuse to serve files we don't understand */
        mimetype = get_mimetype(buf);
        if (!mimetype) {
            lwsl_err("Unknown mimetype for %s\n", buf);
            lws_return_http_status(wsi,
                      HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE, NULL);
            return -1;
        }

        /* demonstrates how to set a cookie on / */

        other_headers = leaf_path;
        p = (unsigned char *)leaf_path;
        if (!strcmp((const char *)in, "/") &&
               !lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_COOKIE)) {
            /* this isn't very unguessable but it'll do for us */
            gettimeofday(&tv, NULL);
            n = sprintf(b64, "test=LWS_%u_%u_COOKIE;Max-Age=360000",
                (unsigned int)tv.tv_sec,
                (unsigned int)tv.tv_usec);

            if (lws_add_http_header_by_name(wsi,
                (unsigned char *)"set-cookie:",
                (unsigned char *)b64, n, &p,
                (unsigned char *)leaf_path + sizeof(leaf_path)))
                return 1;
        }
        if (lws_is_ssl(wsi) && lws_add_http_header_by_name(wsi,
                        (unsigned char *)
                        "Strict-Transport-Security:",
                        (unsigned char *)
                        "max-age=15768000 ; "
                        "includeSubDomains", 36, &p,
                        (unsigned char *)leaf_path +
                            sizeof(leaf_path)))
            return 1;
        n = (char *)p - leaf_path;

        n = lws_serve_http_file(wsi, buf, mimetype, other_headers, n);
        if (n < 0 || ((n > 0) && lws_http_transaction_completed(wsi)))
            return -1; /* error or can't reuse connection: close the socket */

        /*
         * notice that the sending of the file completes asynchronously,
         * we'll get a LWS_CALLBACK_HTTP_FILE_COMPLETION callback when
         * it's done
         */
        break;

    case LWS_CALLBACK_HTTP_BODY:
        strncpy(buf, (const char *)in, 20);
        buf[20] = '\0';
        if (len < 20)
            buf[len] = '\0';

        lwsl_notice("LWS_CALLBACK_HTTP_BODY: %s... len %d\n",
                (const char *)buf, (int)len);

        break;

    case LWS_CALLBACK_HTTP_BODY_COMPLETION:
        lwsl_notice("LWS_CALLBACK_HTTP_BODY_COMPLETION\n");
        /* the whole of the sent body arrived, close or reuse the connection */
        lws_return_http_status(wsi, HTTP_STATUS_OK, NULL);
        goto try_to_reuse;

    case LWS_CALLBACK_HTTP_FILE_COMPLETION:
        goto try_to_reuse;

    case LWS_CALLBACK_HTTP_WRITEABLE:
        lwsl_info("LWS_CALLBACK_HTTP_WRITEABLE\n");

        if (pss->fd == LWS_INVALID_FILE)
            goto try_to_reuse;

        /*
         * we can send more of whatever it is we were sending
         */
        sent = 0;
        do {
            /* we'd like the send this much */
            n = sizeof(buffer) - LWS_PRE;

            /* but if the peer told us he wants less, we can adapt */
            m = lws_get_peer_write_allowance(wsi);

            /* -1 means not using a protocol that has this info */
            if (m == 0)
                /* right now, peer can't handle anything */
                goto later;

            if (m != -1 && m < n)
                /* he couldn't handle that much */
                n = m;

            n = lws_plat_file_read(wsi, pss->fd,
                           &amount, buffer + LWS_PRE, n);
            /* problem reading, close conn */
            if (n < 0) {
                lwsl_err("problem reading file\n");
                goto bail;
            }
            n = (int)amount;
            /* sent it all, close conn */
            if (n == 0)
                goto penultimate;
            /*
             * To support HTTP2, must take care about preamble space
             *
             * identification of when we send the last payload frame
             * is handled by the library itself if you sent a
             * content-length header
             */
            m = lws_write(wsi, buffer + LWS_PRE, n, LWS_WRITE_HTTP);
            if (m < 0) {
                lwsl_err("write failed\n");
                /* write failed, close conn */
                goto bail;
            }
            if (m) /* while still active, extend timeout */
                lws_set_timeout(wsi, PENDING_TIMEOUT_HTTP_CONTENT, 5);
            sent += m;

        } while (!lws_send_pipe_choked(wsi) && (sent < 1024 * 1024));
later:
        lws_callback_on_writable(wsi);
        break;
penultimate:
        lws_plat_file_close(wsi, pss->fd);
        pss->fd = LWS_INVALID_FILE;
        goto try_to_reuse;

bail:
        lws_plat_file_close(wsi, pss->fd);

        return -1;

    /*
     * callback for confirming to continue with client IP appear in
     * protocol 0 callback since no websocket protocol has been agreed
     * yet.  You can just ignore this if you won't filter on client IP
     * since the default uhandled callback return is 0 meaning let the
     * connection continue.
     */
    case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:

        /* if we returned non-zero from here, we kill the connection */
        break;

    /*
     * callbacks for managing the external poll() array appear in
     * protocol 0 callback
     */

    case LWS_CALLBACK_LOCK_POLL:
        /*
         * lock mutex to protect pollfd state
         * called before any other POLL related callback
         * if protecting wsi lifecycle change, len == 1
         */
        //test_server_lock(len);
        break;

    case LWS_CALLBACK_UNLOCK_POLL:
        /*
         * unlock mutex to protect pollfd state when
         * called after any other POLL related callback
         * if protecting wsi lifecycle change, len == 1
         */
        //test_server_unlock(len);
        break;

#ifdef EXTERNAL_POLL
    case LWS_CALLBACK_ADD_POLL_FD:

        if (count_pollfds >= max_poll_elements) {
            lwsl_err("LWS_CALLBACK_ADD_POLL_FD: too many sockets to track\n");
            return 1;
        }

        fd_lookup[pa->fd] = count_pollfds;
        pollfds[count_pollfds].fd = pa->fd;
        pollfds[count_pollfds].events = pa->events;
        pollfds[count_pollfds++].revents = 0;
        break;

    case LWS_CALLBACK_DEL_POLL_FD:
        if (!--count_pollfds)
            break;
        m = fd_lookup[pa->fd];
        /* have the last guy take up the vacant slot */
        pollfds[m] = pollfds[count_pollfds];
        fd_lookup[pollfds[count_pollfds].fd] = m;
        break;

    case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
            pollfds[fd_lookup[pa->fd]].events = pa->events;
        break;
#endif

    case LWS_CALLBACK_GET_THREAD_ID:
        /*
         * if you will call "lws_callback_on_writable"
         * from a different thread, return the caller thread ID
         * here so lws can use this information to work out if it
         * should signal the poll() loop to exit and restart early
         */

        /* return pthread_getthreadid_np(); */

        break;

    default:
        break;
    }

    return 0;

    /* if we're on HTTP1.1 or 2.0, will keep the idle connection alive */
try_to_reuse:
    if (lws_http_transaction_completed(wsi))
        return -1;

    return 0;
}
