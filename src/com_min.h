#pragma once

#define MB_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

MB_GUID(MB_IID_IDXGIDevice,          0x54ec77fa, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c);
MB_GUID(MB_IID_IDXGIFactory2,        0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0);
MB_GUID(MB_IID_IDXGISwapChain2,      0xa8be2ac4, 0x199f, 0x4946, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7);
MB_GUID(MB_IID_ID3D11Texture2D,      0x6f15aaf2, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c);
MB_GUID(MB_IID_IDCompositionDevice,  0xc37ea93a, 0xe7aa, 0x450d, 0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3);
MB_GUID(MB_IID_IDWriteFactory,       0xb859ee5a, 0xd838, 0x4b5b, 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48);
MB_GUID(MB_IID_IDWriteFactory2,      0x0439fc60, 0xca44, 0x4994, 0x8d, 0xee, 0x3a, 0x9a, 0xf7, 0xb7, 0x32, 0xec);

typedef struct DW_FONT_METRICS {
    UINT16 designUnitsPerEm;
    UINT16 ascent;
    UINT16 descent;
    INT16  lineGap;
    UINT16 capHeight;
    UINT16 xHeight;
    INT16  underlinePosition;
    UINT16 underlineThickness;
    INT16  strikethroughPosition;
    UINT16 strikethroughThickness;
} DW_FONT_METRICS;

typedef struct DW_GLYPH_METRICS {
    INT32  leftSideBearing;
    UINT32 advanceWidth;
    INT32  rightSideBearing;
    INT32  topSideBearing;
    UINT32 advanceHeight;
    INT32  bottomSideBearing;
    INT32  verticalOriginY;
} DW_GLYPH_METRICS;

typedef struct DW_GLYPH_OFFSET {
    FLOAT advanceOffset;
    FLOAT ascenderOffset;
} DW_GLYPH_OFFSET;

typedef struct DWFontFace DWFontFace;

typedef struct DW_GLYPH_RUN {
    DWFontFace            *fontFace;
    FLOAT                  fontEmSize;
    UINT32                 glyphCount;
    const UINT16          *glyphIndices;
    const FLOAT           *glyphAdvances;
    const DW_GLYPH_OFFSET *glyphOffsets;
    BOOL                   isSideways;
    UINT32                 bidiLevel;
} DW_GLYPH_RUN;

enum {
    DW_RENDERING_MODE_NATURAL_SYMMETRIC = 5,
    DW_MEASURING_MODE_NATURAL = 0,
    DW_GRID_FIT_MODE_DEFAULT = 0,
    DW_TEXT_ANTIALIAS_MODE_GRAYSCALE = 1,
    DW_TEXTURE_ALIASED_1x1 = 0,
    DW_TEXTURE_CLEARTYPE_3x1 = 1,
    DW_FONT_STRETCH_NORMAL = 5,
    DW_FONT_STYLE_NORMAL = 0,
};

typedef struct DWFactory DWFactory;
typedef struct DWFontCollection DWFontCollection;
typedef struct DWFontFamily DWFontFamily;
typedef struct DWFont DWFont;
typedef struct DWGlyphRunAnalysis DWGlyphRunAnalysis;

typedef struct DWFactoryVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DWFactory *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DWFactory *);
    ULONG   (STDMETHODCALLTYPE *Release)(DWFactory *);
    HRESULT (STDMETHODCALLTYPE *GetSystemFontCollection)(DWFactory *, DWFontCollection **, BOOL);   // 3
    void *pad4_22[19];                                                                                // 4..22
    HRESULT (STDMETHODCALLTYPE *CreateGlyphRunAnalysis)(DWFactory *, const DW_GLYPH_RUN *, FLOAT pixelsPerDip,
                                                        const void *transform, UINT32 renderingMode, UINT32 measuringMode,
                                                        FLOAT originX, FLOAT originY, DWGlyphRunAnalysis **);  // 23
    void *pad24_29[6];                                                                                // 24..29
    HRESULT (STDMETHODCALLTYPE *CreateGlyphRunAnalysis2)(DWFactory *, const DW_GLYPH_RUN *, const void *transform,
                                                         UINT32 renderingMode, UINT32 measuringMode, UINT32 gridFitMode,
                                                         UINT32 antialiasMode, FLOAT originX, FLOAT originY,
                                                         DWGlyphRunAnalysis **);                      // 30
} DWFactoryVtbl;
struct DWFactory { const DWFactoryVtbl *lpVtbl; };

typedef struct DWFontCollectionVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DWFontCollection *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DWFontCollection *);
    ULONG   (STDMETHODCALLTYPE *Release)(DWFontCollection *);
    UINT32  (STDMETHODCALLTYPE *GetFontFamilyCount)(DWFontCollection *);
    HRESULT (STDMETHODCALLTYPE *GetFontFamily)(DWFontCollection *, UINT32, DWFontFamily **);
    HRESULT (STDMETHODCALLTYPE *FindFamilyName)(DWFontCollection *, const WCHAR *, UINT32 *, BOOL *);
} DWFontCollectionVtbl;
struct DWFontCollection { const DWFontCollectionVtbl *lpVtbl; };

typedef struct DWFontFamilyVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DWFontFamily *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DWFontFamily *);
    ULONG   (STDMETHODCALLTYPE *Release)(DWFontFamily *);
    void *pad3_6[4];                                                                                  // 3..6
    HRESULT (STDMETHODCALLTYPE *GetFirstMatchingFont)(DWFontFamily *, UINT32 weight, UINT32 stretch, UINT32 style, DWFont **); // 7
} DWFontFamilyVtbl;
struct DWFontFamily { const DWFontFamilyVtbl *lpVtbl; };

typedef struct DWFontVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DWFont *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DWFont *);
    ULONG   (STDMETHODCALLTYPE *Release)(DWFont *);
    void *pad3_12[10];                                                                                // 3..12
    HRESULT (STDMETHODCALLTYPE *CreateFontFace)(DWFont *, DWFontFace **);                             // 13
} DWFontVtbl;
struct DWFont { const DWFontVtbl *lpVtbl; };

typedef struct DWFontFaceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DWFontFace *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DWFontFace *);
    ULONG   (STDMETHODCALLTYPE *Release)(DWFontFace *);
    void *pad3_7[5];                                                                                  // 3..7
    void    (STDMETHODCALLTYPE *GetMetrics)(DWFontFace *, DW_FONT_METRICS *);                         // 8
    UINT16  (STDMETHODCALLTYPE *GetGlyphCount)(DWFontFace *);                                         // 9
    HRESULT (STDMETHODCALLTYPE *GetDesignGlyphMetrics)(DWFontFace *, const UINT16 *, UINT32, DW_GLYPH_METRICS *, BOOL); // 10
    HRESULT (STDMETHODCALLTYPE *GetGlyphIndices)(DWFontFace *, const UINT32 *, UINT32, UINT16 *);    // 11
} DWFontFaceVtbl;
struct DWFontFace { const DWFontFaceVtbl *lpVtbl; };

typedef struct DWGlyphRunAnalysisVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DWGlyphRunAnalysis *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DWGlyphRunAnalysis *);
    ULONG   (STDMETHODCALLTYPE *Release)(DWGlyphRunAnalysis *);
    HRESULT (STDMETHODCALLTYPE *GetAlphaTextureBounds)(DWGlyphRunAnalysis *, UINT32 textureType, RECT *);             // 3
    HRESULT (STDMETHODCALLTYPE *CreateAlphaTexture)(DWGlyphRunAnalysis *, UINT32 textureType, const RECT *, BYTE *, UINT32); // 4
} DWGlyphRunAnalysisVtbl;
struct DWGlyphRunAnalysis { const DWGlyphRunAnalysisVtbl *lpVtbl; };

typedef HRESULT (WINAPI *PFN_DWriteCreateFactory)(UINT32 factoryType, REFIID iid, IUnknown **factory);

typedef struct DCDevice DCDevice;
typedef struct DCTarget DCTarget;
typedef struct DCVisual DCVisual;

typedef struct DCDeviceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DCDevice *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DCDevice *);
    ULONG   (STDMETHODCALLTYPE *Release)(DCDevice *);
    HRESULT (STDMETHODCALLTYPE *Commit)(DCDevice *);                                    // 3
    HRESULT (STDMETHODCALLTYPE *WaitForCommitCompletion)(DCDevice *);                   // 4
    void *pad5;                                                                         // 5 GetFrameStatistics
    HRESULT (STDMETHODCALLTYPE *CreateTargetForHwnd)(DCDevice *, HWND, BOOL, DCTarget **); // 6
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(DCDevice *, DCVisual **);                 // 7
} DCDeviceVtbl;
struct DCDevice { const DCDeviceVtbl *lpVtbl; };

typedef struct DCTargetVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DCTarget *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DCTarget *);
    ULONG   (STDMETHODCALLTYPE *Release)(DCTarget *);
    HRESULT (STDMETHODCALLTYPE *SetRoot)(DCTarget *, DCVisual *);                       // 3
} DCTargetVtbl;
struct DCTarget { const DCTargetVtbl *lpVtbl; };

typedef struct DCVisualVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(DCVisual *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(DCVisual *);
    ULONG   (STDMETHODCALLTYPE *Release)(DCVisual *);
    void *pad3_14[12];                                                                  // 3..14
    HRESULT (STDMETHODCALLTYPE *SetContent)(DCVisual *, IUnknown *);                    // 15
} DCVisualVtbl;
struct DCVisual { const DCVisualVtbl *lpVtbl; };

typedef HRESULT (WINAPI *PFN_DCompositionCreateDevice)(IDXGIDevice *, REFIID, void **);

// d3dcompiler_47.dll is part of Windows 10+, loaded at runtime.
typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defines,
                                         ID3DInclude *include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                         ID3DBlob **code, ID3DBlob **errors);
