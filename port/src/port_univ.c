/* port_univ.c — replacements for the UnivUtilities.c routines the compiled
 * game files call. The original file pokes low-memory globals (mouse warp,
 * menu bar height), so it is replaced rather than compiled; the pure
 * functions below are transcribed from Sources/UnivUtilities.c verbatim.
 */

#include "shim_internal.h"

/* ---- transcribed from UnivUtilities.c ---- */

void PasStringCopy (StringPtr p1, StringPtr p2)
{
	short stringLength;
	stringLength = *p2++ = *p1++;
	while (--stringLength >= 0)
		*p2++ = *p1++;
}

void PasStringCopyNum (StringPtr p1, StringPtr p2, short charsToCopy)
{
	short charsCopied;
	if (charsToCopy > *p1)
		charsToCopy = *p1;
	*p2 = (unsigned char)charsToCopy;
	p2++;
	p1++;
	charsCopied = charsToCopy;
	while (--charsCopied >= 0)
		*p2++ = *p1++;
}

void PasStringConcat (StringPtr p1, StringPtr p2)
{
	short i, n = *p1;
	for (i = 1; i <= n; i++)
	{
		if (p2[0] >= 255) break;
		p2[0]++;
		p2[p2[0]] = p1[i];
	}
}

short RandomInt (short range)
{
	long rawResult;
	rawResult = Random();
	if (rawResult < 0)
		rawResult *= -1;
	return (short)(rawResult * (long)range / 32768);
}

Boolean RandomCoin (void)
{
	return (Boolean)(Random() & 0x00000001);
}

Boolean ForcePointInRect (Point *thePoint, Rect *theBounds)
{
	Boolean pointMoved = FALSE;
	if (thePoint->h < theBounds->left) { thePoint->h = theBounds->left; pointMoved = TRUE; }
	else if (thePoint->h > theBounds->right) { thePoint->h = theBounds->right; pointMoved = TRUE; }
	if (thePoint->v < theBounds->top) { thePoint->v = theBounds->top; pointMoved = TRUE; }
	else if (thePoint->v > theBounds->bottom) { thePoint->v = theBounds->bottom; pointMoved = TRUE; }
	return pointMoved;
}

void GetOrigin (Point *theOrigin)
{
	theOrigin->h = 0;
	theOrigin->v = 0;
}

Boolean KeyIsDown (short keyCodeOffset)
{
	KeyMap km;
	GetKeys(km);
	return (Boolean)(BitTst(&km, keyCodeOffset) != 0);
}

Boolean CommandKeyIsDown (void) { return KeyIsDown(48); }
Boolean OptionKeyIsDown (void) { return KeyIsDown(61); }
Boolean CommandPeriodDown (void)
{
	KeyMap km;
	GetKeys(km);
	return (Boolean)(BitTst(&km, 48) && BitTst(&km, 40));
}

void GetChooserName (StringPtr thisName)
{
	static const unsigned char def[] = "\006Player";
	memcpy(thisName, def, sizeof def);
}

/* ---- errors: log; fatal ones exit ---- */

void DeathError (short errorCode)
{
	ShimLog("FATAL error %d", errorCode);
	exit(1);
}

void MinorError (short errorCode)
{
	ShimLog("minor error %d (dialog suppressed)", errorCode);
}

void SimpleMessage (void) {}

/* ---- menu bar / dialog helpers: gone in the port ---- */

void ShowMenuBar (WindowPtr w) { (void)w; }
void HideMenuBar (WindowPtr w) { (void)w; }
void CenterDialog (short id) { (void)id; }
void CenterAlert (short id) { (void)id; }
void ZoomOutDialogRect (short id) { (void)id; }
void HoldIt (short ticks)
{
	long dummy;
	Delay(ticks, &dummy);
}

void USSecsToDateString (long secs, StringPtr dateString)
{
	(void)secs;
	dateString[0] = 0;
}
void IntlSecsToDateString (long secs, StringPtr dateString) { USSecsToDateString(secs, dateString); }

OSErr GetRect (Rect *theRect, short resID)
{
	(void)resID;
	theRect->top = theRect->left = theRect->bottom = theRect->right = 0;
	return -1;
}
