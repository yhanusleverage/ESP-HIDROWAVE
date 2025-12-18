/**
 * @file ObjectPoolManager.cpp
 * @brief Implementación de ObjectPoolManager - Definiciones de miembros estáticos
 */

#include "ObjectPoolManager.h"

// ✅ Definiciones de miembros estáticos (una sola vez para evitar múltiples definiciones)
ObjectPoolManager* ObjectPoolManager::instance = nullptr;
SemaphoreHandle_t ObjectPoolManager::instanceMutex = nullptr;

