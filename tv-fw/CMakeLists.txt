# BIOMA — fondo de pantalla vivo en el ESP32-P4 (WT9932P4-TINY, chip v1.0).
# Vida artificial Physarum en los dos núcleos, escalado por PPA, JPEG por hardware y webcam UVC 1080p.
# El juez de la evolución es el tamaño del JPEG. Prototipo del laboratorio de ideas: compilar sí, flashear sólo cuando se decida.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Clase de video (UVC) de TinyUSB: esp_tinyusb no la trae en su configuración, se activa aquí
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_VIDEO=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_VIDEO_STREAMING=1" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=32768" APPEND)
project(bioma_p4)
