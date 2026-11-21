package main

import "math"

// haversineKm is the great-circle distance in kilometres.
func haversineKm(lat1, lon1, lat2, lon2 float64) float64 {
	const r = 6371.0088
	p := math.Pi / 180
	dLat, dLon := (lat2-lat1)*p, (lon2-lon1)*p
	a := math.Sin(dLat/2)*math.Sin(dLat/2) + math.Cos(lat1*p)*math.Cos(lat2*p)*math.Sin(dLon/2)*math.Sin(dLon/2)
	return 2 * r * math.Asin(math.Sqrt(a))
}

const kmPerNM = 1.852
