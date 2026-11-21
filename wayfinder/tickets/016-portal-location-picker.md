---
type: grilling
status: closed
---

## Question

How does the owner pick the location on the portal?

## Resolution (owner, 21 Nov 2026)

Three ways on one page, as recommended:
- **A map with a draggable pin** and a circle showing the radius. The main way. Map tiles and the map library load in the visitor's browser from the internet, which works on the home network.
- **A search box** for an address or place name, using OpenStreetMap's free search (Nominatim). Light use only (one request per search, never automatic), identified, per its usage policy.
- **Latitude and longitude fields** as a fallback that needs no internet.

The portal also sets radius, minimum and maximum altitude, flight card duration (default 10 s), the on/off switches for flights, weather and headlines, one active layout per screen, the headline queries, and shows the Wi-Fi network with a forget button.

Open: radius units (kilometres proposed, aviation uses nautical miles), portal access control (LAN only today, no password), timezone and 12/24 h for the idle clock.
