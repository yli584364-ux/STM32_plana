#pragma once

#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

void registerWebHandlers(WebServer &server, QueueHandle_t displayCommandQueue);