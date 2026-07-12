
#ifndef _GECKO_H_
#define _GECKO_H_

#ifdef __cplusplus
extern "C"
{
#endif

	char ascii(char s);

#ifndef NO_DEBUG
	//use this just like printf();
	void gprintf(const char *str, ...);
	bool InitGecko();
	void hexdump(void *d, int len);
	void USBGeckoOutput();
	/* Per-game runtime toggle. `on != 0` initialises the USB Gecko on EXI1
	 * (if attached) and lets gprintf/USBGeckoOutput emit; `on == 0` disables
	 * further emission. Wired to the GameCFG.USBGeckoTTY toggle at boot. */
	void EnableGeckoTTY(int on);
#else
#define gprintf(...)
#define InitGecko()	  false
#define hexdump( x, y )
#define USBGeckoOutput()
#define EnableGeckoTTY(on)
#endif /* NO_DEBUG */

#ifdef __cplusplus
}
#endif

#endif

