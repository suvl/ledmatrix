---
type: grilling
status: closed
---

## Question

Where does the owner's data service run, and how is it reached?

## Resolution (owner, 21 Nov 2026, with findings from read-only checks)

- **Built and hosted by the owner at `apis.example.com`**, on the single-node k3s cluster (kube context `k3s`, ssh host `k3s-node`, node address 10.0.0.10).
- **Certificate: not ours to set.** The name is covered by the existing Let's Encrypt wildcard `*.example.com` that cert-manager issues through Cloudflare DNS validation. Verified today: RSA-2048, TLS 1.3, 3-certificate chain, so a handshake in the fast class (about 0.4 s on the board). Chain roots in the standard Mozilla bundle (ISRG), which ticket 019 relies on.
- **Reachability: LAN only.** `apis.example.com` and `home.example.com` resolve to 10.0.0.10, a private address. The board and the owner's phone reach it on the home network; nothing is exposed to the internet.
- **IPv6: not yet.** Neither name has an AAAA record. `k3s-node` has a global IPv6 address in the same /64 as the board. Dual stack on this link needs an AAAA record (the owner's Cloudflare DNS) and the Envoy gateway listening on IPv6. To be verified on the cluster before asking for a DNS change.
- **What the cluster already offers:** Envoy Gateway (class `eg`, gateway `default/gateway`) with per-service `HTTPRoute`s; the wildcard certificate secret `wildcard-cert`; Valkey at `valkey.valkey-system:6379` (usable as a cache); a GitHub Actions runner scale set inside the cluster; existing images under `ghcr.io/suvl/`; `local-path` storage; an MQTT broker.
- **Consequence for the device:** one HTTPS host, RSA certificate, small JSON. With fetching done in a background task, even a slow handshake would not touch the panel; the RSA certificate just keeps first data fast.

Open: implementation language, image build and deploy flow, cache (memory or Valkey), the request and response format, and access control on the service.
