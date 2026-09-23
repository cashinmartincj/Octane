# Deploying Octane

Octane is an origin application server. It should not embed or manage NGINX,
TLS certificates, Kubernetes routing, service discovery, or public-edge
policy. Run those as deployment infrastructure and keep Octane focused on the
application and HTTP transport.

This guide describes the supported deployment contract. It is not a substitute
for the security and reliability requirements of a particular organization.

## Deployment contract

The component in front of Octane must:

- terminate public TLS;
- enforce an edge body limit no larger than `HttpLimits::max_body_bytes`;
- buffer or otherwise normalize bodies so the upstream request is sent with a
  valid Content-Length rather than chunked Transfer-Encoding;
- preserve upstream HTTP/1.1 connections where appropriate;
- apply public rate limiting and coarse denial-of-service controls;
- set forwarding headers only on a network path that clients cannot bypass.

Octane must:

- listen only on an address reachable by the intended proxy or Service;
- use finite request, response-write, and shutdown deadlines;
- expose cheap readiness and liveness endpoints;
- keep blocking work off ring threads;
- receive `SIGTERM` and enough time to drain before the runtime kills the Pod;
- emit application logs and metrics to the platform's collection path.

## Recommended Kubernetes topology

Use an Octane Deployment behind a ClusterIP Service. Route external traffic to
that Service with a maintained Gateway API implementation or ingress
controller:

```text
Internet
   |
Gateway/controller: TLS, limits, buffering, routing
   |
ClusterIP Service
   |
   +-- Octane Pod
   +-- Octane Pod
   +-- Octane Pod
```

In this topology, the gateway and Octane are in different Pods. Octane must
bind to the Pod network interface:

```cpp
octane::transport::TcpServerOptions transport;
transport.bind_address = "0.0.0.0";
transport.pin_workers = false; // Prefer orchestrator-controlled placement.

app.listen(8080, explicit_worker_count, limits, transport);
```

Do not bind to `127.0.0.1`: another Pod cannot reach that loopback listener.
Protect the ClusterIP Service with NetworkPolicy so only the selected gateway
and authorized internal clients can reach it.

The following manifests show the application-side shape. Replace the image,
labels, resources, worker configuration, probe path, and security policy for
the actual environment:

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: octane-app
spec:
  replicas: 3
  selector:
    matchLabels:
      app: octane-app
  template:
    metadata:
      labels:
        app: octane-app
    spec:
      terminationGracePeriodSeconds: 15
      containers:
        - name: app
          image: registry.example.com/octane-app:release-id
          ports:
            - name: http
              containerPort: 8080
          resources:
            requests:
              cpu: "1"
              memory: 256Mi
            limits:
              cpu: "1"
              memory: 512Mi
          readinessProbe:
            httpGet:
              path: /health
              port: http
            periodSeconds: 5
          livenessProbe:
            httpGet:
              path: /health
              port: http
            periodSeconds: 10
          securityContext:
            allowPrivilegeEscalation: false
            runAsNonRoot: true
            capabilities:
              drop: ["ALL"]
---
apiVersion: v1
kind: Service
metadata:
  name: octane-app
spec:
  type: ClusterIP
  selector:
    app: octane-app
  ports:
    - name: http
      port: 8080
      targetPort: http
```

The exact Gateway, HTTPRoute, buffering, and body-limit configuration depends
on the chosen implementation. Test it rather than assuming all controllers
reframe request bodies identically.

The Kubernetes community `kubernetes/ingress-nginx` controller was retired on
March 24, 2026 and no longer receives security fixes. Do not select it for a
new deployment. This is separate from other NGINX-based commercial or
community products. See the
[official retirement notice](https://kubernetes.io/blog/2025/11/11/ingress-nginx-retirement/)
and evaluate a maintained Gateway API implementation or controller.

## NGINX sidecar topology

Containers in the same Pod share a network namespace. If NGINX is deliberately
deployed as a sidecar in every application Pod, Octane may bind to loopback:

```text
Service -> Pod NGINX :80 -> 127.0.0.1:8080 Octane
```

```cpp
transport.bind_address = "127.0.0.1";
```

This couples NGINX scaling and resource usage to every application replica.
Prefer the shared gateway plus ClusterIP design unless per-Pod proxying is an
explicit requirement.

## NGINX in a separate Pod

A plain NGINX Deployment in separate Pods must reach Octane through the
ClusterIP Service:

```nginx
upstream octane_origin {
    server octane-app:8080;
    keepalive 64;
}

location / {
    client_max_body_size 1m;
    proxy_request_buffering on;
    proxy_buffering on;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_pass http://octane_origin;
}
```

Use the Service DNS name, not `127.0.0.1`. The template at
[`../deploy/nginx/octane.conf`](../deploy/nginx/octane.conf) targets a same-host
origin and must be adapted before use in separate Pods.

## Traditional host or VM

When NGINX and Octane run on one host, bind Octane to loopback:

```cpp
transport.bind_address = "127.0.0.1";
```

Install the supplied virtual-host template only after replacing its domain and
adding the organization's TLS configuration:

```bash
sudo cp deploy/nginx/octane.conf /etc/nginx/conf.d/octane.conf
sudo nginx -t
sudo systemctl reload nginx
```

Run the application under a process supervisor such as systemd or a container
runtime. Do not depend on an interactive shell to keep it alive.

## CPU and thread sizing

Pass an explicit ring-worker count to `app.listen`. Do not assume
`hardware_concurrency()` equals a container's CPU quota. Start with one worker
per CPU actually allocated to the container and measure under the real traffic
mix.

Queue threads multiply by ring shards. For example, four ring workers and a
named queue configured with two threads per shard can create eight additional
threads when that queue is activated. Include all ring and queue threads in CPU
and memory sizing.

Leave `pin_workers` enabled only when the process has an intentional CPU
affinity set, such as a Kubernetes Guaranteed-QoS Pod using static CPU Manager
allocation. Otherwise disable it and let the orchestrator schedule workers.

`ring_queue_depth` also consumes kernel resources. Larger is not automatically
faster; measure queue depth and locked-memory requirements on the target nodes.

## Runtime and security policy

Before rollout, verify that the target kernel, container runtime, seccomp
profile, and host policy permit `io_uring_setup` and the operations Octane uses.
Do not disable the cluster's security policy globally. Create the narrowest
reviewed workload policy that supports the application.

Run as a non-root user, use a read-only root filesystem where application asset
layout permits it, drop Linux capabilities, avoid host networking, and do not
publish the Octane container port directly with NodePort or hostPort unless
that exposure is an explicit design choice.

## Health and shutdown

Provide a cheap inline endpoint that does not depend on a saturated blocking
queue:

```cpp
void health(octane::HttpRequest&, octane::HttpResponse& res) {
    res.status(200).json(R"({"status":"ok"})");
}

app.get("/health", health);
```

A readiness check may include essential dependency state, but a liveness check
should avoid restarting healthy processes merely because a remote dependency
is temporarily unavailable.

Octane handles `SIGINT` and `SIGTERM`: it stops accepting, closes idle
connections, and lets active requests and responses drain until its shutdown
policy advances cancellation. Set `terminationGracePeriodSeconds` comfortably
above `HttpLimits::shutdown_timeout` plus expected platform routing delay.
Queue workers cannot preempt handler code that never returns.

## Forwarded addresses and trust

Octane does not independently authenticate forwarding headers. Read
`X-Forwarded-For`, `X-Forwarded-Proto`, or vendor headers only when NetworkPolicy
and Service exposure prevent clients from reaching Octane directly. Define the
trusted-proxy chain in the application rather than accepting the leftmost
address blindly.

## Observability

The framework does not provide a production logging or metrics subsystem.
Applications should emit, at minimum:

- request count, status class, and latency;
- active connections and rejected/saturated work where available;
- queue saturation and dependency latency;
- process CPU, memory, file descriptors, and restart count;
- structured errors without secrets, credentials, or raw sensitive bodies;
- readiness state and graceful-shutdown events.

Use a request/correlation ID supplied by a trusted edge or generate one in the
application. Keep metrics labels bounded; do not use raw paths containing IDs
as high-cardinality labels.

## Rollout verification

At minimum, verify each release with:

1. Release compilation and the complete CTest suite.
2. Startup under the actual runtime and security profile.
3. Readiness transition before traffic is admitted.
4. GET and bounded POST requests through the real gateway.
5. A client-supplied chunked POST through the gateway, confirming the upstream
   request reaches Octane with Content-Length or is rejected at the edge.
6. Header and body limits through the gateway and directly on a test network.
7. Queue saturation behavior and expected 503 responses.
8. Pod deletion during active traffic, confirming graceful termination.
9. Horizontal scaling and load distribution across replicas.
10. A sustained soak on the target kernel followed by error, latency, memory,
    file-descriptor, and restart review.

## Production readiness boundaries

Octane is not a full HTTP conformance implementation. It does not terminate
TLS, decode chunked bodies, implement HTTP/2 or HTTP/3, parse application JSON,
provide authentication, or cap application response size. No multi-hour soak
or full conformance audit is claimed by the repository.

A deployment is ready only when the application and edge supply those missing
responsibilities, the target runtime checks pass, and the observed workload
stays within measured resource and latency budgets.
