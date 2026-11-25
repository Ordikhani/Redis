#pragma once
#include <QtCore/qglobal.h>
#if defined(ARS_STATIC)
# define ARS_EXPORT
#elif defined(ARS_LIBRARY)
# define ARS_EXPORT Q_DECL_EXPORT
#else
# define ARS_EXPORT Q_DECL_IMPORT
#endif
