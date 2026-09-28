# libhttp

Echo's standard library has files, streams, and threads. It does not have sockets. Sometimes your program still needs to *be* the HTTP server. This module is that missing piece: a listen socket, a reactor, a worker pool, and a route table, with `Request` and `Response` as ordinary values.

The C is a small POSIX shim (`bind` / `listen` / `accept` / `recv` / `send`, and kqueue or epoll). Parsing, routing, the reactor, and the threads are Echo. There is no libcurl in here, and there is no TLS. libcurl is the client. This is the other direction.

## A complete pass

Let's look at a whole program, so the rest of the page has somewhere to sit:

```echo
use http::{Server, Request, Response};

Server $app = Server();

$app->get('/', function(Request $req) : Response {
    return Response(text: 'hello');
});

int32 $port = guard $app->listen('127.0.0.1', 8080) else ($e) {
    die("listen failed: {$e}");
}
```

That is the whole shape: one `Server`, some routes, and `listen`. A handler takes a `Request` and returns a `Response`.

`listen` binds, starts the workers, and runs the server on the calling thread. It returns the bound port after `stop()`, or an `Error` if bind failed. Port `0` is ephemeral, which is how the tests find a free one.

## Installing

From your project directory:

```bash
epm add echolang/libhttp --git https://github.com/echolang/libhttp --range ^0.2
```

That writes a `#[requires:]` line and vendors the sources. The C shim travels with the module, so you never write a `#[cc:]` yourself.

POSIX only: darwin and linux, matching Echo's supported targets. There is no Winsock in v1.

## Routes

You attach a handler with `on`, or with the verb methods that call it. `get`, `post`, `put`, `patch`, `del`, `head`, `options`. `del` rather than `delete`, which reads as a keyword in most of the languages nearby.

```echo
$app->get('/hello', function(Request $req) : Response {
    return Response(text: 'hello');
});

$app->post('/echo', function(Request $req) : Response {
    return Response(ok: $req->body);
});

$app->on(http::Method::other('COPY'), '/file', function(Request $req) : Response {
    return Response::noContent();
});
```

A verb this library does not name is still a route. `.other('COPY')` is that leftover.

First match wins, registration order. `{name}` is a whole path segment:

```echo
$app->get('/users/{id}', function(Request $req) : Response {
    string $id = $req->param('id') ?? '';

    return Response(text: $id);
});
```

`/users/{id}` matches `/users/7`. It does not match `/users/7/edit`, and it does not match `/users/` either: a param is a whole segment, and an empty one is not there.

The path is split on `/` first, then each segment is percent-decoded. `/users/a%20b` hands the handler `a b`, and `%2F` stays inside its segment instead of becoming a new one. A literal segment matches its decoded spelling too. `+` in a path is a plus; only the query string reads it as a space.

### Optional, rest, and checked

`{name}` is one whole segment. Three more shapes live in the same braces: one segment or none, everything left, and a segment that has to pass a check.

```echo
$app->get('/posts/{page?}', ...);          // /posts and /posts/2
$app->get('/files/{path*}', ...);          // /files, /files/a, /files/a/b/c
$app->get('/users/{id:number}', ...);      // /users/7, not /users/me
```

`{page?}` takes one segment or none. Left out, the param is not set, so the default goes where you read it: `$req->param('page') ?? '1'`. `/posts/` is still not `/posts`: an empty segment is not a missing one.

`{path*}` takes every segment left, zero or more, joined back with `/`. It is always set, `''` when the path stopped at it. `/{path*}` catches everything, `/` included. The segments were decoded first, so a `%2F` inside one reads like any other slash here. `$req->path` still has the raw bytes.

`{name:kind}` is one segment that has to pass a check. When it fails, that route is not a match and the next route gets its turn:

| Kind | Takes |
|---|---|
| `number` | ASCII digits, no sign |
| `alpha` | ASCII letters |
| `alphanum` | ASCII letters and digits |
| `uuid` | `8-4-4-4-12` hex, either case |
| `ulid` | 26 Crockford base32, either case, first digit `0`..`7` |
| `draft\|live` | exactly one of the listed values, as written |

The check runs on the decoded segment. It goes after the `?`: `{page?:number}`. A `{name*}` takes no check, since it has no one segment to check. There is no regex, because Echo has none to give.

`?` and `*` only go last. Registering `/a/{rest*}/b`, a kind that does not exist (`{id:numbr}`), or a name used twice stops the program at `on`, with the pattern in the message. A route that can never mean anything is a bug in the program, not a request to answer.

Order still decides. With a check, `/users/{id:number}` can sit in front of `/users/me`. A rest route goes last, or it answers for everything registered after it.

`listen` snapshots the table. A later `on` changes a copy nobody is looking at.

### HEAD

If you did not register HEAD, a matching GET handler runs and the body is stripped on the wire. The `Content-Length` is still the length GET would have sent. That is HTTP, not a courtesy.

### 404 and 405

No route matches the path: 404. The path matches a different method: 405 with `Allow`. You do not write those handlers. The server does.

## Middleware

Logging, auth, CORS, request IDs: things every request needs, and no handler should have to remember. `wrap` runs a function around all of them:

```echo
$app->wrap(function(Request $req, http::Next $next) : Response {
    time::instant $start = time::instant::now();
    Response $res = $next->run($req);

    echo "{$req->method} {$req->path} {$res->status} {$start->elapsed()}";
    return $res;
});
```

`$next->run($req)` is the rest of the chain: the middleware registered after this one, then routing, then the handler. What it answers is the response, and you can change it on the way out. Not calling it is how a middleware answers on its own:

```echo
$app->wrap(function(Request $req, http::Next $next) : Response {
    if ($req->header('Authorization') == null) {
        return Response(401, 'Unauthorized');
    }

    return $next->run($req);
});
```

The first `wrap` is the outermost. It sees the request first and the response last. `wrap` rather than `use`, which Echo keeps for imports.

The chain ends in routing, so middleware sees every request that parsed, 404 and 405 included. That is what lets a CORS layer answer a preflight `OPTIONS` nobody registered, and a log line record a miss. It does not see a request that never parsed (400, 408, 413, 501, 505). There is no `Request` to hand it.

Params are filled by routing, after middleware ran, so `$req->param('id')` is empty in middleware and set in the handler. A middleware that rewrites `$req->path` before calling `$next->run` changes which route matches. Like routes, middleware is snapshotted at `listen`.

## Request

A `Request` exists when the bytes parsed. "Did the server start" and "was this request well-formed" are different questions. A truncated body, a missing Host on HTTP/1.1, a Content-Length that is not plain digits or disagrees with another, a header line with no colon or with whitespace before it (`Host : x`), a missing request line: those are 400s on that connection, not an `Error` from `listen`. HTTP/1.0 may omit Host.

```echo
$req->method          // http::Method
$req->path            // /search
$req->target          // /search?q=echo, as it arrived
$req->queryString     // q=echo
$req->query           // http::Query, a list of pairs
$req->headers         // http::Headers
$req->body            // string, binary safe
$req->peer            // 127.0.0.1:54321
$req->version         // HTTP/1.1
```

Helpers:

```echo
$req->header('Content-Type')    // string?, case-insensitive
$req->param('id')               // string?, from `{id}`, `{id?}`, `{id*}`
$req->query->get('page')        // string?, first value
```

A body comes one of two ways: a `Content-Length`, or `Transfer-Encoding: chunked`, which is how a client uploads something it has not finished producing. A chunked body is decoded as it arrives, and `$req->body` is the decoded bytes. The headers stay as they arrived, `Transfer-Encoding` included. Trailers are checked and then dropped. The decoded size counts against `maxBody`.

Chunked framing is read strictly. Every line ends in CRLF, a chunk size is at most 16 hex digits, and `chunked` has to be the last coding and appear once. A request that sends both `Transfer-Encoding` and `Content-Length` is a 400. Two servers that each believe a different one is how a request gets smuggled past the first. So is a `Transfer-Encoding` on HTTP/1.0, which has no transfer codings.

Two refusals get their own status. A coding other than `chunked` (`gzip`, `identity`) is 501, since this server does not decode it. An `HTTP/x.y` other than `HTTP/1.0` and `HTTP/1.1` is 505.

`Expect: 100-continue` is answered for you. Once the head parses and the length fits `maxBody`, the server sends `HTTP/1.1 100 Continue` and then reads the body, so a client like curl does not sit out its own timeout first. A chunked upload gets its 100 too. A body that is too large gets its 413 instead of the 100. Any other `Expect` is 417. HTTP/1.0 has no 100, so its `Expect` is ignored.

The handler takes `Request` by value. That is a copy. It is also the type the compiler can store in a `function<Response(Request)>` without crashing. Shared mutable state does not live on the request. It lives on an `#[atomic]` class the closure captures, the same way `std::thread` tells you to share anything.

```echo
#[atomic]
public class Hits
{
    public atomic<int32> $n;

    public constructor()
    {
        $this->n = atomic<int32>(0);
    }
}

Hits $hits = Hits();

$app->get('/hits', function(Request $req) : Response {
    $hits->n->add(1);
    return Response(text: str::from($hits->n->load()));
});
```

## Response

Public fields. The handler fills them. Several construction paths are labelled constructors, and the parameterless ones are statics, which the leading dot reaches at a typed destination:

```echo
return Response(200, $body);
return Response(ok: $body);            // 200, no Content-Type of our own
return Response(text: 'hello');        // 200, text/plain; charset=utf-8
return Response(json: '{"ok":true}');  // 200, application/json. a string in, no libjson
return Response(html: $page);
return .notFound();                    // 404
return .badRequest();                  // 400
return .noContent();                   // 204
```

`notFound`, `badRequest`, and `noContent` are statics because there is no argument to label. A labelled constructor is not a static, so `return .text('hi');` does not compile: write `Response(text: 'hi')`.

```echo
Response $res = Response(text: 'hello');
$res->header('X-Request-Id', 'abc');
```

`$res->header($name, $value)` replaces. `$res->headers->add` keeps duplicates.

The serializer owns the framing. It writes `Content-Length` from the body, and whether the connection stays open. A `Content-Length`, `Transfer-Encoding`, `Connection`, or `Keep-Alive` you set yourself is dropped: a wrong length hangs the client, and whether a connection is kept is the server's call. 1xx, 204, and 304 are sent with no body and no length, whatever the body field holds. Every response carries a `Date` from the wall clock, unless you set one yourself, in which case yours goes out instead. A header whose name is empty or holds a colon, or whose name or value holds CR, LF, or NUL, is dropped rather than written: it would split the response or forge another header.

`"{$res}"` is the short form, `200 (5 bytes)`, which is useful in a log and useless as a parser.

### Keep-alive

A connection carries more than one request. HTTP/1.1 keeps it unless the client says `Connection: close`. HTTP/1.0 closes it unless the client says `Connection: keep-alive`, and then gets told yes, and for how long (`Keep-Alive: timeout=5`). Requests sent back to back without waiting (pipelining) are answered in order.

The server closes a kept connection after `idleTimeout` with nothing sent, after `maxRequests` requests, and once it is stopping. Any error response (400, 408, 413, and the rest) closes too. The last response on a connection says `Connection: close`, so the client knows before it tries another.

An idle connection costs a file descriptor, not a worker. See [Threads](#threads).

## Streaming

`$body` is a string, which is fine until the body is a 2 GB file, or a stream of events that never ends. For those, the handler hands over a callback instead, and the body is written while the response goes out:

```echo
$app->get('/numbers', function(Request $req) : Response {
    return Response(stream: function(http::Writer $w) : void {
        int32 $i = 0;

        while ($i < 1000 && $w->open()) {
            $w->write("{$i}\n");
            $i = $i + 1;
        }
    });
});
```

Over HTTP/1.1 that goes out chunked, and the connection is kept. An HTTP/1.0 client has no chunked coding, so it gets the bytes as they are and the close is the end of the body. When you know the length up front, say so. It goes out as `Content-Length`, bytes past it are dropped, and a stream that falls short closes the connection, since the client is still waiting for the rest:

```echo
return Response(stream: $fn, length: 1048576);
```

`write` buffers up to 4 KiB and sends when that fills. `flush` sends what is buffered now, which is what server-sent events need after each message:

```echo
$app->get('/events', function(Request $req) : Response {
    Response $res = Response(stream: function(http::Writer $w) : void {
        int32 $tick = 0;

        while ($w->open()) {
            $w->write("data: {$tick}\n\n");
            $w->flush();
            thread::sleep(1000);
            $tick = $tick + 1;
        }
    });

    $res->header('Content-Type', 'text/event-stream');
    $res->header('Cache-Control', 'no-cache');
    return $res;
});
```

`open()` goes false once a write failed, meaning the client is gone, and once the server is stopping. A long stream should loop on it, so `stop()` is not left waiting on an endless one. `write` and `flush` answer the same thing as a `bool`.

Files are a stream with a known length:

```echo
return Response(file: '/srv/site/index.html');
```

That opens the file, sets `Content-Length` from its size and `Content-Type` from its extension, and streams it from disk in 64 KiB reads. A missing file or a directory is a 404, and one the process may not read is a 403, decided when the `Response` is built. The path is used as given. Keeping a request from naming something outside what you meant to serve (`..`, an absolute path) is the handler's job.

A HEAD, or a status that never has a body, sends the head and never runs the callback. A HEAD keeps the length when there is one.

A stream runs on the worker that answered the request, and holds it until the callback returns. So `workers` is also how many streams can run at once. It runs after the handler and every middleware have returned, so a middleware that times `$next->run` measures the handler, not the transfer. A stream's `$body` is ignored, and `$res->streamed()` says which kind a response is. The `Writer` only works during the callback. Keep no copy of it past the callback's return.

## Headers and query

**An `array` with a linear scan. Not a `map<string, string>`.** A map would compile. It would also hash exact bytes (so case-insensitive lookup would allocate a lowercase key on every insert *and* every read), drop every `Set-Cookie` after the first, and `get` would assert on a miss, which is gone in a release build.

`str::iequals` is the comparison HTTP actually wants. Repeats are kept. Order is arrival order.

```echo
echo $req->header('Content-Type') ?? '(none)';
array<string> $cookies = $req->headers->all('cookie');
```

`Query` is the same shape for `a=1&b=2`. `+` is a space, then `%XX` is a byte. A `%` that is not two hex digits is kept as written, which is the forgiving reading every browser takes.

## Threads

`listen` runs two kinds of thread. The calling thread becomes the reactor: it owns the listen socket and every connection that is idle or still sending its request, and waits on all of them at once (kqueue on darwin, epoll on linux). Once a request is complete, it goes to a worker. The pool's default size is `thread::concurrency()`, at least one.

```echo
$app->workers(4);
$app->maxBody(1024 * 1024);                // default 1 MiB
$app->headerTimeout(.secs(10));            // default 10 seconds
$app->bodyTimeout(.secs(60));              // default 60 seconds
$app->idleTimeout(.secs(5));               // default 5 seconds
$app->maxRequests(100);                    // default 100 per connection
$app->maxConnections(1024);                // default 1024
$app->maxQueue(256);                       // default 256
```

That split is the point. A worker only ever sees a whole request, and gives the connection back the moment its response is written. A browser tab holding a kept connection, or a client sending its headers a byte at a time, costs the reactor a slot and costs no worker anything. So `workers` bounds how many handlers run at once, not how many clients can be connected. The two things that do hold a worker are a handler running and a stream writing.

A `Content-Length` above `maxBody` is 413 without reading the rest.

Each timeout is one deadline for the whole phase, not a per-read allowance. A client that sends one byte every few seconds still runs out at 10 seconds and gets a 408. The header clock starts at the first byte. Before that the connection is idle, and `idleTimeout` closes it without an answer, the same as a kept connection between requests. Deadlines are checked every 100 ms, so one can fire up to that much late. Each send to a slow reader is capped at 10 seconds too.

Complete requests wait in a queue until a worker is free. A new connection is answered `503 Service Unavailable` with `Retry-After: 1`, straight from the reactor, once `maxQueue` requests are waiting or `maxConnections` connections are open. A flood costs a refusal instead of a longer and longer line. Every connection is a file descriptor, so keep `maxConnections` under the process's `ulimit -n`. If the process runs out anyway, the server stops accepting for a moment instead of spinning on it.

A connection the server closes after answering gets a lingering close: the server shuts its side, drains what the client is still sending for up to two seconds, then closes. That is what lets a client mid-upload read its 413 instead of a reset. The 503 is the exception. It is a plain close, because draining on the reactor would stall everyone else.

`stop()` is safe from another thread. The reactor wakes, closes every idle connection, and stops accepting. Requests already on a worker finish, and their connections close after the response. Then `listen` returns the bound port. `workers(0)` fails with `Error::tooFewWorkers` before bind.

A handler that `die`s ends the process, not just its worker. So does a failed `assert`, a `guard` with no `else`, or an index past the end of an array. Echo has no catch, and a `crash::` hook can report but not recover. There is no half-alive pool to repair. When a request is bad, return an error `Response` instead of stopping, and run a production server under something that restarts it: systemd's `Restart=`, launchd's `KeepAlive`, or your container runtime.

## Failures

`listen` answers `result<int32, Error>`. The value is the bound port. Bind failing is the thing you `guard`:

```echo
int32 $port = guard $app->listen('127.0.0.1', 8080) else ($e) {
    match ($e) {
        .bind($m) => {
            die("could not bind: {$m}");
        },
        .badPort => {
            die('port must be 0..65535');
        },
        else => {
            die("{$e}");
        },
    }
}
```

`Error` is an enum. `"{$e}"` is the sentence. A malformed *client* request is not this type: it is a 400 on that connection.

## Testing

```bash
echoc test                        # everything
echoc test --filter group:unit    # no network
echoc test --filter group:net     # loopback only
```

The unit tests need no network at all. Parsing, routing, percent-decoding, and response rendering are pure Echo. The net tests bind `127.0.0.1:0`, send a request over a real socket, and `stop()`.

The router has a benchmark. It is compiled only with the `bench` flag, so a plain `echoc test` never runs it:

```bash
echoc test --release --optimize whole --define bench --filter group:bench --verbose
```

Measure with `--optimize whole`. Under the default, `module`, the stdlib does not inline into this module, and a five-byte string compare costs 13 ns instead of 2. That turns every number into a benchmark of call overhead. Build your server the same way.

## Examples

```bash
echoc run -m . examples/hello.eco     # GET / on 127.0.0.1:8080
echoc run -m . examples/rest.eco      # /users/{id:number}, /posts/{page?}, POST /echo, a catch-all
echoc run -m . examples/stream.eco    # a request log, /events, /count, /readme
```

## What is not here yet

- **TLS.** Plain HTTP. Terminate TLS in front, or wait.
- **HTTP/2, websockets.** HTTP/1.1 only. Keep-alive and chunked are in, `Upgrade` is not.
- **Transfer codings other than chunked.** A `gzip` upload is 501, and responses are never compressed.
- **IPv6 and Windows.** IPv4, POSIX. Echo's prebuilts are darwin arm64 and linux x86_64.
- **A static file server, cookies, multipart.** `Response(file:)` serves one file you named. Mapping a URL onto a directory, safely, is not in here. A JSON body is a string. libjson is how it becomes a value.
- **Ranges.** No `Range`, no 206. A download that breaks starts over.
- **A body cap the server enforces on the response.** Requests have `maxBody`. What you write back is yours.

## Source layout

| File | What it holds |
|---|---|
| `c/posix.c` | sockets, kqueue / epoll, the wake pipe. The only C |
| `src/sys.eco` | the one `extern` block |
| `src/error.eco` `from.eco` | `Error`, `WireError`, and `"{$e}"` |
| `src/method.eco` | `Method` |
| `src/headers.eco` `query.eco` | `Headers`, `Query`, percent-decoding |
| `src/request.eco` `response.eco` | the two values, and `Response(file:)` |
| `src/parse.eco` `chunked.eco` | the `Framer`: request bytes in, a piece at a time |
| `src/render.eco` `writer.eco` | response bytes out, and the `Writer` a stream writes through |
| `src/route.eco` `middleware.eco` | pattern compiling, matching, 405 `Allow`, and `wrap` |
| `src/conn.eco` | owned socket |
| `src/reactor.eco` | the thread that holds idle and half-read connections |
| `src/worker.eco` | a whole request in, a response out |
| `src/server.eco` | `Server`, its settings, `listen` / `stop` |
| `src/text.eco` | the ASCII bytes and defaults this module compares against |
