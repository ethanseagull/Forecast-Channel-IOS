// Port: the Bluetooth stack is not built. Only the plain types that the
// public SC/WUD/WPAD headers mention are kept here.
#ifndef RVL_SDK_PUBLIC_BTE_H
#define RVL_SDK_PUBLIC_BTE_H
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned long UINT32;
typedef unsigned char BOOLEAN;
#define BD_ADDR_LEN 6
typedef UINT8 BD_ADDR[BD_ADDR_LEN];
typedef UINT8* BD_ADDR_PTR;
#define LINK_KEY_LEN 16
typedef UINT8 LINK_KEY[LINK_KEY_LEN];
typedef UINT16 tBTA_HH_ATTR_MASK;
#endif
