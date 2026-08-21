# Deploying

GitHub Actions builds the image; Flux deploys it. Neither half lives in both places.

| | Where | What |
|---|---|---|
| Image | this repo, `.github/workflows/docker-publish.yml` | builds `server/Dockerfile` with the repo root as context (it needs `pfrm/` too) and pushes `ghcr.io/markmckessock/photoframe:latest` |
| Manifests | `kube-saturn`, `kubernetes/apps/home-automation/photoframe-webhook/` | the Flux `Kustomization`, `HelmRelease`, `ExternalSecret`, PVC and gatus endpoint |

The manifests are **not** duplicated here — the cluster repo is the single source of
truth for what is running, the same as every other app in it.

Because the tag is `latest`, Kubernetes defaults `imagePullPolicy` to `Always`, so
shipping a new image is:

```bash
git push                                    # CI builds and pushes ghcr.io/.../photoframe:latest
kubectl -n home-automation rollout restart deploy/photoframe-webhook
```

A push alone does not redeploy: nothing watches the registry.

## Before the first apply

**1. Create the 1Password item** named `photoframe-webhook` with three fields:

| Field | Value |
|---|---|
| `TWILIO_ACCOUNT_SID` | `AC…` |
| `TWILIO_AUTH_TOKEN` | from the Twilio console |
| `TWILIO_WEBHOOK_URL` | `https://photoframe.markmckessock.com/mms` — exact, character for character |

That is one field more than `splitflap-webhook` needs, and only one. That service just
*receives* webhooks, so the auth token alone is enough to verify a signature. This one
fetches the photo back from `api.twilio.com` afterwards, and that request authenticates
as `AccountSid:AuthToken` — without the SID there is no image.

`TWILIO_WEBHOOK_URL` is the usual first thing to get wrong: the signature is computed
over that string, so a trailing slash, `http` instead of `https`, or a stale hostname
makes **every** request 403.

### Optional: an allowlist and SMS admin commands

`ALLOWED_NUMBERS` and `ADMIN_NUMBERS` are read from the environment and both default to
empty, which means anyone who knows the number can send a photo and nobody can run
`/status` or `/refresh` by text. That is the same posture `splitflap-webhook` runs in.

If you want either, add the field to the 1Password item **and** the matching line to
`app/externalsecret.yaml`. Do not put them in the `HelmRelease` as plain env: they are
phone numbers and `kube-saturn` is a public repo.

Add them in that order, too. `template.data` renders `{{ .ADMIN_NUMBERS }}` against the
extracted item, so referencing a field that does not exist yet fails the render and the
**whole Secret** stops syncing — including the Twilio credentials.

**2. Pick a free LoadBalancer address.** `app/helmrelease.yaml` asks for `10.0.70.133`
in the Cilium IPAM pool (mosquitto holds `.131`). Check nothing else has it, then put
the same address in `IMAGE_URL` in `firmware/src/secrets.h`.

**3. Point Twilio at it.** In the console, set the number's incoming-message webhook to
`https://photoframe.markmckessock.com/mms`, method POST.

## What is exposed where

This is the part worth being careful about.

| Path | Reachable from | Why |
|---|---|---|
| `/mms` | the internet, via `envoy-external` | Twilio has to reach it. It authenticates itself with a request signature. |
| `/latest.pfrm` | **LAN only** | It is a photo a friend sent you. |
| `/latest.png` | LAN only | Preview of what the panel will show. |
| `/status`, `/healthz` | LAN only | |

`kube-saturn/CLAUDE.md` notes that everything on `*.markmckessock.com` behind the
Cloudflare tunnel is public by default. That is why the `HTTPRoute` matches
`PathPrefix: /mms` and nothing else, and why the image endpoints are served through a
separate LoadBalancer Service that has no route attached at all. Both Services point at
the same pod — the isolation is in the routing, so **if you widen that path match, you
publish the photos.**

## Checking it

```bash
kubectl -n home-automation logs -l app.kubernetes.io/name=photoframe-webhook -f

# From the LAN, as the frame sees it:
curl -sI http://10.0.70.133/latest.pfrm             # 200, ETag, Content-Length 960064
curl -sI -H 'If-None-Match: "<that etag>"' http://10.0.70.133/latest.pfrm   # 304
open http://10.0.70.133/latest.png                  # what the panel will show

# And confirm the photo is NOT public:
curl -sI https://photoframe.markmckessock.com/latest.pfrm   # must be 404 from the gateway
```

## Mosquitto

No changes needed. Keeping the megabyte on HTTP rather than in a retained MQTT message
is precisely why the broker's `100Mi` limit is still fine.
