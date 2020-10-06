// NameTableViewer.cpp
#include <stdio.h>
#include <stdint.h>

#include <QDir>
#include <QPainter>
#include <QInputDialog>
#include <QMessageBox>

#include "../../types.h"
#include "../../fceu.h"
#include "../../cart.h"
#include "../../ppu.h"
#include "../../ines.h"
#include "../../debug.h"
#include "../../palette.h"

#include "Qt/NameTableViewer.h"
#include "Qt/main.h"
#include "Qt/dface.h"
#include "Qt/input.h"
#include "Qt/config.h"
#include "Qt/fceuWrapper.h"

static ppuNameTableViewerDialog_t *nameTableViewWindow = NULL;
static uint8_t palcache[36]; //palette cache
static int NTViewScanline = 0;
static int NTViewSkip = 100;
static int NTViewRefresh = 1;
static int chrchanged = 0;

static int xpos = 0, ypos = 0;
static int attview = 0;
static int hidepal = 0;
static bool redrawtables = true;

// checkerboard tile for attribute view
static const uint8_t ATTRIBUTE_VIEW_TILE[16] = { 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };


static class NTCache 
{
public:
	NTCache(void) 
		: curr_vnapage(0)
	{}

	uint8_t* curr_vnapage;
	uint8_t cache[0x400];
} cache[4];

static ppuNameTable_t nameTable[4];

enum NT_MirrorType 
{
	NT_NONE = -1,
	NT_HORIZONTAL, NT_VERTICAL, NT_FOUR_SCREEN,
	NT_SINGLE_SCREEN_TABLE_0, NT_SINGLE_SCREEN_TABLE_1,
	NT_SINGLE_SCREEN_TABLE_2, NT_SINGLE_SCREEN_TABLE_3,
	NT_NUM_MIRROR_TYPES
};
static NT_MirrorType ntmirroring, oldntmirroring = NT_NONE;

static void initNameTableViewer(void);
//----------------------------------------------------
int openNameTableViewWindow( QWidget *parent )
{
	if ( nameTableViewWindow != NULL )
	{
		return -1;
	}
	initNameTableViewer();

	nameTableViewWindow = new ppuNameTableViewerDialog_t(parent);

	nameTableViewWindow->show();

	return 0;
}
//----------------------------------------------------
ppuNameTableViewerDialog_t::ppuNameTableViewerDialog_t(QWidget *parent)
	: QDialog( parent )
{
	QVBoxLayout *mainLayout, *vbox;
	QHBoxLayout *hbox;
	QGridLayout *grid;
	QGroupBox   *frame;
	char stmp[64];

	nameTableViewWindow = this;

   setWindowTitle( tr("Name Table Viewer") );

	mainLayout = new QVBoxLayout();

	setLayout( mainLayout );

	vbox   = new QVBoxLayout();
	frame  = new QGroupBox( tr("Name Tables") );
	ntView = new ppuNameTableView_t(this);
	grid   = new QGridLayout();

	vbox->addWidget( ntView );
	frame->setLayout( vbox );
	mainLayout->addWidget( frame, 100 );
	mainLayout->addLayout( grid ,   1 );

	showScrollLineCbox = new QCheckBox( tr("Show Scroll Lines") );
	showAttrbCbox      = new QCheckBox( tr("Show Attributes") );
	ignorePaletteCbox  = new QCheckBox( tr("Ignore Palette") );

	grid->addWidget( showScrollLineCbox, 0, 0, Qt::AlignLeft );
	grid->addWidget( showAttrbCbox     , 1, 0, Qt::AlignLeft );
	grid->addWidget( ignorePaletteCbox , 2, 0, Qt::AlignLeft );

	hbox   = new QHBoxLayout();
	refreshSlider  = new QSlider( Qt::Horizontal );
	hbox->addWidget( new QLabel( tr("Refresh: More") ) );
	hbox->addWidget( refreshSlider );
	hbox->addWidget( new QLabel( tr("Less") ) );
	grid->addLayout( hbox, 0, 1, Qt::AlignRight );

	refreshSlider->setMinimum( 0);
	refreshSlider->setMaximum(25);
	refreshSlider->setValue(NTViewRefresh);

	connect( refreshSlider, SIGNAL(valueChanged(int)), this, SLOT(refreshSliderChanged(int)));

	hbox         = new QHBoxLayout();
	scanLineEdit = new QLineEdit();
	hbox->addWidget( new QLabel( tr("Display on Scanline:") ) );
	hbox->addWidget( scanLineEdit );
	grid->addLayout( hbox, 1, 1, Qt::AlignRight );

	scanLineEdit->setMaxLength( 3 );
	scanLineEdit->setInputMask( ">900;" );
	sprintf( stmp, "%i", NTViewScanline );
	scanLineEdit->setText( tr(stmp) );

	connect( scanLineEdit, SIGNAL(textEdited(const QString &)), this, SLOT(scanLineChanged(const QString &)));

	FCEUD_UpdateNTView( -1, true);
	
	updateTimer  = new QTimer( this );

   connect( updateTimer, &QTimer::timeout, this, &ppuNameTableViewerDialog_t::periodicUpdate );

	updateTimer->start( 33 ); // 30hz
}
//----------------------------------------------------
ppuNameTableViewerDialog_t::~ppuNameTableViewerDialog_t(void)
{
	updateTimer->stop();
	nameTableViewWindow = NULL;

	printf("Name Table Viewer Window Deleted\n");
}
//----------------------------------------------------
void ppuNameTableViewerDialog_t::closeEvent(QCloseEvent *event)
{
   printf("Name Table Viewer Close Window Event\n");
   done(0);
	deleteLater();
   event->accept();
}
//----------------------------------------------------
void ppuNameTableViewerDialog_t::closeWindow(void)
{
   printf("Close Window\n");
   done(0);
	deleteLater();
}
//----------------------------------------------------
void ppuNameTableViewerDialog_t::periodicUpdate(void)
{

	if ( redrawtables )
	{
		this->update();
		redrawtables = false;
	}
}
//----------------------------------------------------
void ppuNameTableViewerDialog_t::scanLineChanged( const QString &txt )
{
	std::string s;

	s = txt.toStdString();

	if ( s.size() > 0 )
	{
		NTViewScanline = strtoul( s.c_str(), NULL, 10 );
	}
	//printf("ScanLine: '%s'  %i\n", s.c_str(), PPUViewScanline );
}
//----------------------------------------------------
void ppuNameTableViewerDialog_t::refreshSliderChanged(int value)
{
	NTViewRefresh = value;
}
//----------------------------------------------------
ppuNameTableView_t::ppuNameTableView_t(QWidget *parent)
	: QWidget(parent)
{
	this->setFocusPolicy(Qt::StrongFocus);
	this->setMouseTracking(true);
	viewWidth = 256 * 2;
	viewHeight = 240 * 2;
	setMinimumWidth( viewWidth );
	setMinimumHeight( viewHeight );
}
//----------------------------------------------------
ppuNameTableView_t::~ppuNameTableView_t(void)
{

}
//----------------------------------------------------
void ppuNameTableView_t::resizeEvent(QResizeEvent *event)
{
	viewWidth  = event->size().width();
	viewHeight = event->size().height();

	//printf("%ix%i\n", viewWidth, viewHeight );
}
//----------------------------------------------------
void ppuNameTableView_t::mouseMoveEvent(QMouseEvent *event)
{
	//QPoint tile = convPixToTile( event->pos() );

	//if ( (tile.x() < 16) && (tile.y() < 16) )
	//{
	//	char stmp[64];
	//	sprintf( stmp, "Tile: $%X%X", tile.y(), tile.x() );
	//	tileLabel->setText( tr(stmp) );
	//}
}
//----------------------------------------------------------------------------
void ppuNameTableView_t::mousePressEvent(QMouseEvent * event)
{
	//QPoint tile = convPixToTile( event->pos() );

	if ( event->button() == Qt::LeftButton )
	{
	}
	else if ( event->button() == Qt::RightButton )
	{
	}
}
//----------------------------------------------------
void ppuNameTableView_t::paintEvent(QPaintEvent *event)
{
	ppuNameTable_t *nt;
	int n,i,j,ii,jj,w,h,x,y,xx,yy;
	QPainter painter(this);
	viewWidth  = event->rect().width();
	viewHeight = event->rect().height();

	w = viewWidth / (256*2);
  	h = viewHeight / (240*2);

	//printf("%ix%i\n", viewWidth, viewHeight );

	xx = 0; yy = 0;

	for (n=0; n<4; n++)
	{
		nt = &nameTable[n];
		
		xx = (n%2) * (viewWidth / 2);
		yy = (n/2) * (viewHeight / 2);

		for (j=0; j<30; j++)
		{
			jj = (j*8);

			for (i=0; i<32; i++)
			{
				ii = (i*8);

				for (y=0; y<8; y++)
				{
					for (x=0; x<8; x++)
					{
						painter.fillRect( xx+(ii+x)*w, yy+(jj+y)*h, w, h, nt->tile[j][i].pixel[y][x].color );
					}
				}
			}
		}
	}
}
//----------------------------------------------------
static void initNameTableViewer(void)
{
	//clear cache
	memset(palcache,0,32);

	// forced palette (e.g. for debugging nametables when palettes are all-black)
	palcache[(8*4)+0] = 0x0F;
	palcache[(8*4)+1] = 0x00;
	palcache[(8*4)+2] = 0x10;
	palcache[(8*4)+3] = 0x20;

}
//----------------------------------------------------
static void ChangeMirroring(void)
{
	switch (ntmirroring)
	{
		case NT_HORIZONTAL:
			vnapage[0] = vnapage[1] = &NTARAM[0x000];
			vnapage[2] = vnapage[3] = &NTARAM[0x400];
			break;
		case NT_VERTICAL:
			vnapage[0] = vnapage[2] = &NTARAM[0x000];
			vnapage[1] = vnapage[3] = &NTARAM[0x400];
			break;
		case NT_FOUR_SCREEN:
			vnapage[0] = &NTARAM[0x000];
			vnapage[1] = &NTARAM[0x400];
			if(ExtraNTARAM)
			{
				vnapage[2] = ExtraNTARAM;
				vnapage[3] = ExtraNTARAM + 0x400;
			}
			break;
		case NT_SINGLE_SCREEN_TABLE_0:
			vnapage[0] = vnapage[1] = vnapage[2] = vnapage[3] = &NTARAM[0x000];
			break;
		case NT_SINGLE_SCREEN_TABLE_1:
			vnapage[0] = vnapage[1] = vnapage[2] = vnapage[3] = &NTARAM[0x400];
			break;
		case NT_SINGLE_SCREEN_TABLE_2:
			if(ExtraNTARAM)
				vnapage[0] = vnapage[1] = vnapage[2] = vnapage[3] = ExtraNTARAM;
			break;
		case NT_SINGLE_SCREEN_TABLE_3:
			if(ExtraNTARAM)
				vnapage[0] = vnapage[1] = vnapage[2] = vnapage[3] = ExtraNTARAM + 0x400;
			break;
		default:
		case NT_NONE:
			break;
	}
	return;
}
//----------------------------------------------------
inline void DrawChr( ppuNameTableTile_t *tile, const uint8_t *chr, int pal)
{
	int y, x, tmp, index=0, p=0;
	uint8 chr0, chr1;
	//uint8 *table = &VPage[0][0]; //use the background table
	//pbitmap += 3*

	for (y = 0; y < 8; y++) { //todo: use index for y?
		chr0 = chr[index];
		chr1 = chr[index+8];
		tmp=7;
		for (x = 0; x < 8; x++) { //todo: use tmp for x?
			p = (chr0>>tmp)&1;
			p |= ((chr1>>tmp)&1)<<1;
			p = palcache[p+(pal*4)];
			tmp--;

			tile->pixel[y][x].color.setBlue( palo[p].b );
			tile->pixel[y][x].color.setGreen( palo[p].g );
			tile->pixel[y][x].color.setRed( palo[p].r );
		}
		index++;
		//pbitmap += (NTWIDTH*3)-24;
	}
	//index+=8;
	//pbitmap -= (((PALETTEBITWIDTH>>2)<<3)-24);
}
//----------------------------------------------------
static void DrawNameTable(int scanline, int ntnum, bool invalidateCache) 
{
	NTCache &c = cache[ntnum];
	uint8_t *tablecache = c.cache;

	uint8_t *table = vnapage[ntnum];
	if (table == NULL)
	{
		table = vnapage[ntnum&1];
	}

	int a, ptable=0;
	
	if (PPU[0]&0x10){ //use the correct pattern table based on this bit
		ptable=0x1000;
	}

	bool invalid = invalidateCache;
	//if we werent asked to invalidate the cache, maybe we need to invalidate it anyway due to vnapage changing
	if (!invalid)
	{
		invalid = (c.curr_vnapage != vnapage[ntnum]);
	}
	c.curr_vnapage = vnapage[ntnum];
	
	//HACK: never cache anything
	invalid = true;

	for (int y=0;y<30;y++)
	{
		for (int x=0;x<32;x++)
		{
			int ntaddr = (y*32)+x;
			int attraddr = 0x3C0+((y>>2)<<3)+(x>>2);
			if (invalid
				|| (table[ntaddr] != tablecache[ntaddr]) 
				|| (table[attraddr] != tablecache[attraddr])) 
			{
				int temp = (((y&2)<<1)+(x&2));
				a = (table[attraddr] & (3<<temp)) >> temp;
				
				//the commented out code below is all allegedly equivalent to the single line above:
				//tmpx = x>>2;
				//tmpy = y>>2;
				//a = 0x3C0+(tmpy*8)+tmpx;
				//if((((x>>1)&1) == 0) && (((y>>1)&1) == 0)) a = table[a]&0x3;
				//if((((x>>1)&1) == 1) && (((y>>1)&1) == 0)) a = (table[a]&0xC)>>2;
				//if((((x>>1)&1) == 0) && (((y>>1)&1) == 1)) a = (table[a]&0x30)>>4;
				//if((((x>>1)&1) == 1) && (((y>>1)&1) == 1)) a = (table[a]&0xC0)>>6;

				int chr = table[ntaddr]*16;

				extern int FCEUPPU_GetAttr(int ntnum, int xt, int yt);

				//test.. instead of pretending that the nametable is a screen at 0,0 we pretend that it is at the current xscroll and yscroll
				//int xpos = ((RefreshAddr & 0x400) >> 2) | ((RefreshAddr & 0x1F) << 3) | XOffset;
				//int ypos = ((RefreshAddr & 0x3E0) >> 2) | ((RefreshAddr & 0x7000) >> 12); 
				//if(RefreshAddr & 0x800) ypos += 240;
				//int refreshaddr = (xpos/8+x)+(ypos/8+y)*32;

				int refreshaddr = (x)+(y)*32;

				a = FCEUPPU_GetAttr(ntnum,x,y);
				if (hidepal) a = 8;

				const uint8* chrp = FCEUPPU_GetCHR(ptable+chr,refreshaddr);
				if (attview) chrp = ATTRIBUTE_VIEW_TILE;

				//a good way to do it:
				DrawChr( &nameTable[ntnum].tile[y][x], chrp, a);

				tablecache[ntaddr] = table[ntaddr];
				tablecache[attraddr] = table[attraddr];
				//one could comment out the line above...
				//since there are so many fewer attribute values than NT values, it might be best just to refresh the whole attr table below with the memcpy

				//obviously this whole scheme of nt cache doesnt work if an mmc5 game is playing tricks with the attribute table
			}
			//pbitmap += (8*3);
		}
		//pbitmap += 7*((NTWIDTH*3));
	}

	 //this copies the attribute tables to the cache if needed. but we arent using it now because 
	//if(redrawtables){
	//	memcpy(tablecache+0x3c0,table+0x3c0,0x40);
	//}
}
//----------------------------------------------------
void FCEUD_UpdateNTView(int scanline, bool drawall) 
{
	if (nameTableViewWindow == 0)
	{
		return;
	}
	if ( (scanline != -1) && (scanline != NTViewScanline) )
	{
		return;
	}

	//uint8 *pbitmap = ppuv_palette;
	//if (!hNTView) return;
	
	ppu_getScroll(xpos,ypos);


	if (NTViewSkip < NTViewRefresh)
	{
		NTViewSkip++;
		return;
	}
	NTViewSkip = 0;

	if (chrchanged)
	{
		drawall = 1;
	}

	//update palette only if required
	if (memcmp(palcache,PALRAM,32) != 0) 
	{
		memcpy(palcache,PALRAM,32);
		drawall = 1; //palette has changed, so redraw all
	}
	 
	ntmirroring = NT_NONE;
	if(vnapage[0] == vnapage[1])ntmirroring = NT_HORIZONTAL;
	if(vnapage[0] == vnapage[2])ntmirroring = NT_VERTICAL;
	if((vnapage[0] != vnapage[1]) && (vnapage[0] != vnapage[2]))ntmirroring = NT_FOUR_SCREEN;

	if((vnapage[0] == vnapage[1]) && (vnapage[1] == vnapage[2]) && (vnapage[2] == vnapage[3])){ 
		if(vnapage[0] == &NTARAM[0x000])ntmirroring = NT_SINGLE_SCREEN_TABLE_0;
		if(vnapage[0] == &NTARAM[0x400])ntmirroring = NT_SINGLE_SCREEN_TABLE_1;
		if(vnapage[0] == ExtraNTARAM)ntmirroring = NT_SINGLE_SCREEN_TABLE_2;
		if(vnapage[0] == ExtraNTARAM+0x400)ntmirroring = NT_SINGLE_SCREEN_TABLE_3;
	}

	if (oldntmirroring != ntmirroring)
	{
		//UpdateMirroringButtons();
		oldntmirroring = ntmirroring;
	}

	for (int i=0;i<4;i++)
	{
		DrawNameTable(scanline,i,drawall);
	}

	chrchanged = 0;
	redrawtables = true;
	return;	
}
//----------------------------------------------------
