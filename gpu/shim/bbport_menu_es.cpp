// SPDX-License-Identifier: GPL-2.0-or-later
// v0.4: the in-game menu in Spanish. MenuText(english, russian) keeps its two languages; with
// menu_language=es the English text is looked up here (missing ones stay in English).
#include <string_view>
#include <unordered_map>

#include "bbport_settings.h"

namespace BbSettings {

const char* Spanish(const char* english) {
    static const std::unordered_map<std::string_view, const char*> table = {
    {"CTR Nitro-Fueled PC  ·  Insert / L3+R3###bbport_settings", "CTR Nitro-Fueled PC  ·  Insert / L3+R3###bbport_settings"},
    {"%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f ms)"},
    {"Off", "Desactivado"},
    {"Output resolution", "Pantalla"},
    {"Aspect / output (restart required)", "Aspecto / salida (tras reiniciar)"},
    {"One choice: the aspect and the output size together. 21:9 centres the HUD at 2560x1080 or 3440x1440; 32:9 at 5120x1440. Upscaling is off while ultrawide is on and internal resolution is capped at 1440p. The first ultrawide launch prepares about 9 GB of patched archives; original files are kept. Applies after restarting.", "Una sola opción: aspecto y salida juntos. 21:9 centra el HUD a 2560x1080 o 3440x1440; 32:9 a 5120x1440. Con ultra-panorámico no hay escalado y la resolución interna llega hasta 1440p. El primer arranque prepara unos 9 GB de archivos; los originales no cambian. Se aplica al reiniciar."},
    {"Auto (based on GPU)", "Automático (según la GPU)"},
    {"Off (faster)", "Desactivado (más rápido)"},
    {"On", "Activado"},
    {"Live resolution changes", "Cambio de resolución en vivo"},
    {"On: output resolution and preset change without restarting, but game post-processing stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. Off: everything renders at the preset resolution, and changes require a restart. Auto enables this on powerful discrete GPUs. Applies after restarting the game.", "Activado: la salida cambia sin reiniciar, pero el posprocesado del juego queda a 1080p (más lento en GPUs modestas). Desactivado: todo se dibuja a la resolución elegida y los cambios piden reiniciar. Automático lo activa en GPUs dedicadas potentes. Se aplica al reiniciar el juego."},
    {"Resolution and upscaling", "Resolución y escalado"},
    {"Game patches (restart required)", "Parches del juego (tras reiniciar)"},
    {"(DLSS quality decides)", "(la decide la calidad de DLSS)"},
    {"(game patches decide)", "(la deciden los parches)"},
    {"Internal resolution", "Resolución interna"},
    {"The size the game draws its 3D scene at. Above 1080p it uses the game's PS4 Pro path and the picture is output at 4K; on a 1080p screen that smooths jagged edges. 4K needs a strong GPU.", "El tamaño al que el juego dibuja la escena 3D. Por encima de 1080p usa el camino de PS4 Pro y la imagen sale en 4K; en una pantalla 1080p suaviza los bordes. 4K necesita una GPU potente."},
    {"Upscaling", "Escalado"},
    {"FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. FSR 3, FSR 4 and DLSS rebuild detail over several frames from the game's depth and motion, and smooth edges even at the same resolution. DLSS needs an NVIDIA RTX card (FSR 3 runs otherwise); FSR 4 needs INT8 dot-product support and its model files. Applies at once.", "FSR 1 agranda una escena más pequeña con bordes más nítidos que el estirado del juego. FSR 3, FSR 4 y DLSS reconstruyen el detalle con varios cuadros y suavizan los bordes incluso a la misma resolución. DLSS necesita una NVIDIA RTX (si no, va FSR 3); FSR 4 necesita soporte INT8. Se aplica al momento."},
    {"Frame generation", "Generación de cuadros"},
    {"Shows an interpolated frame between each pair of game frames: about twice the frames on screen (the game itself, its FPS limit and its timing do not change). Best on 120 Hz+ screens with the game at about half the refresh rate. Works with any upscaler; FSR 3/4 and DLSS also give it the game's motion. Fast HUD elements may shimmer. Applies at once.", "Muestra un cuadro interpolado entre cada par de cuadros del juego: casi el doble de imágenes en pantalla (el juego, su límite de FPS y su ritmo no cambian). Ideal en pantallas de 120 Hz o más con el juego a la mitad. Funciona con cualquier escalado. Se aplica al momento."},
    {"Frame generation motion", "Movimiento para generar"},
    {"Optical flow follows what moves on screen, karts included. The game's motion needs FSR 3/4 or DLSS and moves karts with the camera, which can smear them.", "El flujo óptico sigue lo que se mueve en pantalla, karts incluidos. El movimiento del juego necesita FSR 3/4 o DLSS y mueve los karts con la cámara, lo que puede emborronarlos."},
    {"DLSS quality", "Calidad DLSS"},
    {"Auto keeps the internal resolution above. The others render at NVIDIA's share of the DLSS output: Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%, DLAA 100%. Applies after the restart.", "Automático mantiene la resolución interna de arriba. Las demás dibujan a la proporción de NVIDIA de la salida DLSS: Calidad 67 %, Equilibrado 58 %, Rendimiento 50 %, Ultra rendimiento 33 %, DLAA 100 %. Tras reiniciar."},
    {"DLSS output", "Salida DLSS"},
    {"DLSS model", "Modelo DLSS"},
    {"Transformer: NVIDIA's current model (DLSS 4), sharper and steadier, a little heavier. CNN: the older model (preset E), lighter; NVIDIA marks it deprecated. Applies at once.", "Transformer: el modelo actual de NVIDIA (DLSS 4), más nítido y estable, algo más pesado. CNN: el anterior, más ligero. Se aplica al momento."},
    {"FSR 3 sharpness", "Nitidez FSR 3"},
    {"From patches/CrashTeamRacing.xml (illusionyy/PS-Game-Patch), applied to the executable at start.", "De patches/CrashTeamRacing.xml (illusionyy/PS-Game-Patch); se aplican al arrancar."},
    {"Changes apply after restarting the game", "Los cambios se aplican al reiniciar el juego"},
    {"Apply and restart game", "Aplicar y reiniciar el juego"},
    {"Controllers (split screen)", "Mandos (pantalla dividida)"},
    {"(first / last used)", "(automático: el último que uses)"},
    {"(none)", "(ninguno)"},
    {"Port", "Jugador"},
    {"Ports 2-4 log in as Player 2-4 while their gamepad is connected", "Los jugadores 2 a 4 entran mientras su mando está conectado"},
    {"Performance", "Rendimiento"},
    {"No limit", "Sin límite"},
    {"72 (half of 144 Hz)", "72 (mitad de 144 Hz)"},
    {"FPS limit", "Límite de FPS"},
    {"With the 60 FPS patch the game runs as fast as your PC allows. 60 is the pace it was made for; on a 144 Hz screen without G-Sync/FreeSync, 72 divides the refresh evenly (240 does the same on a 240 Hz screen). Applies at once.", "Con el parche de 60 FPS el juego va tan rápido como permita tu PC. 60 es su ritmo original; en una pantalla de 144 Hz sin G-Sync/FreeSync, 72 reparte los cuadros de forma pareja. Se aplica al momento."},
    {"Compile shaders in the background (experimental)", "Compilar shaders en segundo plano (experimental)"},
    {"A shader seen for the first time no longer pauses the game: an effect may appear a frame or two late instead. Shaders seen before load at start either way. Experimental.", "Un shader nuevo ya no detiene el juego: el efecto puede tardar un cuadro o dos en verse. Los ya vistos cargan al arrancar igual. Experimental."},
    {"Display", "Pantalla"},
    {"Fullscreen", "Pantalla completa"},
    {"Fullscreen on the game's display (BB_DISPLAY). Applies at once; BB_FULLSCREEN=1 at start.", "Pantalla completa en el monitor del juego. Se aplica al momento."},
    {"Other", "Otros"},
    {"FPS counter in corner", "Contador de FPS en la esquina"},
    {"Rescan controllers", "Volver a buscar mandos"},
    {"For a controller connected after the game started and not seen by it.", "Para un mando conectado con el juego ya abierto que no aparece."},
    {"Close", "Cerrar"},
    {"Press again to quit", "Pulsa otra vez para salir"},
    {"Quit game", "Salir del juego"},
    {"Settings are saved to ctrnfpc.ini", "Los ajustes se guardan en ctrnfpc.ini"},
    {"%.0f FPS  %.1f ms  %s", "%.0f FPS  %.1f ms  %s"},
    {"Optical flow (recommended)", "Flujo óptico (recomendado)"},
    {"Game motion + optical flow", "Movimiento del juego + flujo óptico"},
    {"Auto (internal resolution)", "Automático (resolución interna)"},
    {"DLAA (native)", "DLAA (nativo)"},
    {"Quality", "Calidad"},
    {"Balanced", "Equilibrado"},
    {"Ultra Performance", "Ultra rendimiento"},
    {"Transformer (recommended)", "Transformer (recomendado)"},
    {"AMD FSR 4 (experimental)", "AMD FSR 4 (experimental)"},
    {"AMD FSR 3 (experimental)", "AMD FSR 3 (experimental)"},
    {"60 FPS unlock (illusion)", "60 FPS (illusion)"},
    {"Disable motion blur (illusion)", "Sin desenfoque de movimiento (illusion)"},
    {"Picture", "Imagen"},
    {"Game", "Juego"},
    {"Controllers", "Mandos"},
    {"General", "General"},
    {"Language", "Idioma"},
    {"Game patches", "Parches del juego"},
    {"Menu of the port", "Menú del port"},
    };
    const auto it = table.find(english);
    return it == table.end() ? english : it->second;
}

} // namespace BbSettings
