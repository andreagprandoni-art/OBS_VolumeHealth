#pragma once

// Crea il pannello "Audio Health Monitor" dentro OBS
void audio_health_create_dock();

// Ferma la lettura dell'audio (da chiamare in chiusura)
void audio_health_shutdown();
