#pragma once

// Firebase project configuration.
// These values are public (same as in the mobile app's firebase_options.dart).

#define FIREBASE_API_KEY  "AIzaSyD2m4T7ylElcXPbPS9YupoRFX2ebfjB7bI"
#define FIREBASE_DB_HOST  "geyserswitch-bloc-default-rtdb.firebaseio.com"
#define FIREBASE_DB_URL   "https://" FIREBASE_DB_HOST

// Token refresh endpoint.
#define FIREBASE_TOKEN_URL \
    "https://securetoken.googleapis.com/v1/token?key=" FIREBASE_API_KEY
