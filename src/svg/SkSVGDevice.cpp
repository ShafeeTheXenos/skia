/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkSVGDevice.h"

#include "SkBitmap.h"
#include "SkChecksum.h"
#include "SkDraw.h"
#include "SkPaint.h"
#include "SkParsePath.h"
#include "SkPathOps.h"
#include "SkShader.h"
#include "SkStream.h"
#include "SkTHash.h"
#include "SkTypeface.h"
#include "SkUtils.h"
#include "SkXMLWriter.h"

namespace {

static SkString svg_color(SkColor color) {
    return SkStringPrintf("rgb(%u,%u,%u)",
                          SkColorGetR(color),
                          SkColorGetG(color),
                          SkColorGetB(color));
}

static SkScalar svg_opacity(SkColor color) {
    return SkIntToScalar(SkColorGetA(color)) / SK_AlphaOPAQUE;
}

// Keep in sync with SkPaint::Cap
static const char* cap_map[]  = {
    NULL,    // kButt_Cap (default)
    "round", // kRound_Cap
    "square" // kSquare_Cap
};
SK_COMPILE_ASSERT(SK_ARRAY_COUNT(cap_map) == SkPaint::kCapCount, missing_cap_map_entry);

static const char* svg_cap(SkPaint::Cap cap) {
    SkASSERT(cap < SK_ARRAY_COUNT(cap_map));
    return cap_map[cap];
}

// Keep in sync with SkPaint::Join
static const char* join_map[] = {
    NULL,    // kMiter_Join (default)
    "round", // kRound_Join
    "bevel"  // kBevel_Join
};
SK_COMPILE_ASSERT(SK_ARRAY_COUNT(join_map) == SkPaint::kJoinCount, missing_join_map_entry);

static const char* svg_join(SkPaint::Join join) {
    SkASSERT(join < SK_ARRAY_COUNT(join_map));
    return join_map[join];
}

// Keep in sync with SkPaint::Align
static const char* text_align_map[] = {
    NULL,     // kLeft_Align (default)
    "middle", // kCenter_Align
    "end"     // kRight_Align
};
SK_COMPILE_ASSERT(SK_ARRAY_COUNT(text_align_map) == SkPaint::kAlignCount,
                  missing_text_align_map_entry);
static const char* svg_text_align(SkPaint::Align align) {
    SkASSERT(align < SK_ARRAY_COUNT(text_align_map));
    return text_align_map[align];
}

static SkString svg_transform(const SkMatrix& t) {
    SkASSERT(!t.isIdentity());

    SkString tstr;
    switch (t.getType()) {
    case SkMatrix::kPerspective_Mask:
        SkDebugf("Can't handle perspective matrices.");
        break;
    case SkMatrix::kTranslate_Mask:
        tstr.printf("translate(%g %g)", t.getTranslateX(), t.getTranslateY());
        break;
    case SkMatrix::kScale_Mask:
        tstr.printf("scale(%g %g)", t.getScaleX(), t.getScaleY());
        break;
    default:
        // http://www.w3.org/TR/SVG/coords.html#TransformMatrixDefined
        //    | a c e |
        //    | b d f |
        //    | 0 0 1 |
        tstr.printf("matrix(%g %g %g %g %g %g)",
                    t.getScaleX(),     t.getSkewY(),
                    t.getSkewX(),      t.getScaleY(),
                    t.getTranslateX(), t.getTranslateY());
        break;
    }

    return tstr;
}

uint32_t hash_family_string(const SkString& family) {
    // This is a lame hash function, but we don't really expect to see more than 1-2
    // family names under normal circumstances.
    return SkChecksum::Mix(SkToU32(family.size()));
}

struct Resources {
    Resources(const SkPaint& paint)
        : fPaintServer(svg_color(paint.getColor())) {}

    SkString fPaintServer;
    SkString fClip;
};

class SVGTextBuilder : SkNoncopyable {
public:
    SVGTextBuilder(const void* text, size_t byteLen, const SkPaint& paint, const SkPoint& offset,
                   unsigned scalarsPerPos, const SkScalar pos[] = NULL)
        : fOffset(offset)
        , fScalarsPerPos(scalarsPerPos)
        , fPos(pos)
        , fLastCharWasWhitespace(true) // start off in whitespace mode to strip all leading space
    {
        SkASSERT(scalarsPerPos <= 2);
        SkASSERT(scalarsPerPos == 0 || SkToBool(pos));

        int count = paint.countText(text, byteLen);

        switch(paint.getTextEncoding()) {
        case SkPaint::kGlyphID_TextEncoding: {
            SkASSERT(count * sizeof(uint16_t) == byteLen);
            SkAutoSTArray<64, SkUnichar> unichars(count);
            paint.glyphsToUnichars((const uint16_t*)text, count, unichars.get());
            for (int i = 0; i < count; ++i) {
                this->appendUnichar(unichars[i]);
            }
        } break;
        case SkPaint::kUTF8_TextEncoding: {
            const char* c8 = reinterpret_cast<const char*>(text);
            for (int i = 0; i < count; ++i) {
                this->appendUnichar(SkUTF8_NextUnichar(&c8));
            }
            SkASSERT(reinterpret_cast<const char*>(text) + byteLen == c8);
        } break;
        case SkPaint::kUTF16_TextEncoding: {
            const uint16_t* c16 = reinterpret_cast<const uint16_t*>(text);
            for (int i = 0; i < count; ++i) {
                this->appendUnichar(SkUTF16_NextUnichar(&c16));
            }
            SkASSERT(SkIsAlign2(byteLen));
            SkASSERT(reinterpret_cast<const uint16_t*>(text) + (byteLen / 2) == c16);
        } break;
        case SkPaint::kUTF32_TextEncoding: {
            SkASSERT(count * sizeof(uint32_t) == byteLen);
            const uint32_t* c32 = reinterpret_cast<const uint32_t*>(text);
            for (int i = 0; i < count; ++i) {
                this->appendUnichar(c32[i]);
            }
        } break;
        default:
            SkFAIL("unknown text encoding");
        }

        if (scalarsPerPos < 2) {
            SkASSERT(fPosY.isEmpty());
            fPosY.appendScalar(offset.y()); // DrawText or DrawPosTextH (fixed Y).
        }

        if (scalarsPerPos < 1) {
            SkASSERT(fPosX.isEmpty());
            fPosX.appendScalar(offset.x()); // DrawText (X also fixed).
        }
    }

    const SkString& text() const { return fText; }
    const SkString& posX() const { return fPosX; }
    const SkString& posY() const { return fPosY; }

private:
    void appendUnichar(SkUnichar c) {
        bool discardPos = false;
        bool isWhitespace = false;

        switch(c) {
        case ' ':
        case '\t':
            // consolidate whitespace to match SVG's xml:space=default munging
            // (http://www.w3.org/TR/SVG/text.html#WhiteSpace)
            if (fLastCharWasWhitespace) {
                discardPos = true;
            } else {
                fText.appendUnichar(c);
            }
            isWhitespace = true;
            break;
        case '\0':
            // SkPaint::glyphsToUnichars() returns \0 for inconvertible glyphs, but these
            // are not legal XML characters (http://www.w3.org/TR/REC-xml/#charsets)
            discardPos = true;
            isWhitespace = fLastCharWasWhitespace; // preserve whitespace consolidation
            break;
        case '&':
            fText.append("&amp;");
            break;
        case '"':
            fText.append("&quot;");
            break;
        case '\'':
            fText.append("&apos;");
            break;
        case '<':
            fText.append("&lt;");
            break;
        case '>':
            fText.append("&gt;");
            break;
        default:
            fText.appendUnichar(c);
            break;
        }

        this->advancePos(discardPos);
        fLastCharWasWhitespace = isWhitespace;
    }

    void advancePos(bool discard) {
        if (!discard && fScalarsPerPos > 0) {
            fPosX.appendf("%.8g, ", fOffset.x() + fPos[0]);
            if (fScalarsPerPos > 1) {
                SkASSERT(fScalarsPerPos == 2);
                fPosY.appendf("%.8g, ", fOffset.y() + fPos[1]);
            }
        }
        fPos += fScalarsPerPos;
    }

    const SkPoint&  fOffset;
    const unsigned  fScalarsPerPos;
    const SkScalar* fPos;

    SkString fText, fPosX, fPosY;
    bool     fLastCharWasWhitespace;
};

}

// For now all this does is serve unique serial IDs, but it will eventually evolve to track
// and deduplicate resources.
class SkSVGDevice::ResourceBucket : ::SkNoncopyable {
public:
    ResourceBucket() : fGradientCount(0), fClipCount(0), fPathCount(0) {}

    SkString addLinearGradient() {
        return SkStringPrintf("gradient_%d", fGradientCount++);
    }

    SkString addClip() {
        return SkStringPrintf("clip_%d", fClipCount++);
    }

    SkString addPath() {
        return SkStringPrintf("path_%d", fPathCount++);
    }

private:
    uint32_t fGradientCount;
    uint32_t fClipCount;
    uint32_t fPathCount;
};

class SkSVGDevice::AutoElement : ::SkNoncopyable {
public:
    AutoElement(const char name[], SkXMLWriter* writer)
        : fWriter(writer)
        , fResourceBucket(NULL) {
        fWriter->startElement(name);
    }

    AutoElement(const char name[], SkXMLWriter* writer, ResourceBucket* bucket,
                const SkDraw& draw, const SkPaint& paint)
        : fWriter(writer)
        , fResourceBucket(bucket) {

        Resources res = this->addResources(draw, paint);

        fWriter->startElement(name);

        this->addPaint(paint, res);

        if (!draw.fMatrix->isIdentity()) {
            this->addAttribute("transform", svg_transform(*draw.fMatrix));
        }
    }

    ~AutoElement() {
        fWriter->endElement();
    }

    void addAttribute(const char name[], const char val[]) {
        fWriter->addAttribute(name, val);
    }

    void addAttribute(const char name[], const SkString& val) {
        fWriter->addAttribute(name, val.c_str());
    }

    void addAttribute(const char name[], int32_t val) {
        fWriter->addS32Attribute(name, val);
    }

    void addAttribute(const char name[], SkScalar val) {
        fWriter->addScalarAttribute(name, val);
    }

    void addText(const SkString& text) {
        fWriter->addText(text.c_str(), text.size());
    }

    void addRectAttributes(const SkRect&);
    void addPathAttributes(const SkPath&);
    void addTextAttributes(const SkPaint&);

private:
    Resources addResources(const SkDraw& draw, const SkPaint& paint);
    void addClipResources(const SkDraw& draw, Resources* resources);
    void addShaderResources(const SkPaint& paint, Resources* resources);

    void addPaint(const SkPaint& paint, const Resources& resources);

    SkString addLinearGradientDef(const SkShader::GradientInfo& info, const SkShader* shader);

    SkXMLWriter*    fWriter;
    ResourceBucket* fResourceBucket;
};

void SkSVGDevice::AutoElement::addPaint(const SkPaint& paint, const Resources& resources) {
    SkPaint::Style style = paint.getStyle();
    if (style == SkPaint::kFill_Style || style == SkPaint::kStrokeAndFill_Style) {
        this->addAttribute("fill", resources.fPaintServer);

        if (SK_AlphaOPAQUE != SkColorGetA(paint.getColor())) {
            this->addAttribute("fill-opacity", svg_opacity(paint.getColor()));
        }
    } else {
        SkASSERT(style == SkPaint::kStroke_Style);
        this->addAttribute("fill", "none");
    }

    if (style == SkPaint::kStroke_Style || style == SkPaint::kStrokeAndFill_Style) {
        this->addAttribute("stroke", resources.fPaintServer);

        SkScalar strokeWidth = paint.getStrokeWidth();
        if (strokeWidth == 0) {
            // Hairline stroke
            strokeWidth = 1;
            this->addAttribute("vector-effect", "non-scaling-stroke");
        }
        this->addAttribute("stroke-width", strokeWidth);

        if (const char* cap = svg_cap(paint.getStrokeCap())) {
            this->addAttribute("stroke-linecap", cap);
        }

        if (const char* join = svg_join(paint.getStrokeJoin())) {
            this->addAttribute("stroke-linejoin", join);
        }

        if (paint.getStrokeJoin() == SkPaint::kMiter_Join) {
            this->addAttribute("stroke-miterlimit", paint.getStrokeMiter());
        }

        if (SK_AlphaOPAQUE != SkColorGetA(paint.getColor())) {
            this->addAttribute("stroke-opacity", svg_opacity(paint.getColor()));
        }
    } else {
        SkASSERT(style == SkPaint::kFill_Style);
        this->addAttribute("stroke", "none");
    }

    if (!resources.fClip.isEmpty()) {
        this->addAttribute("clip-path", resources.fClip);
    }
}

Resources SkSVGDevice::AutoElement::addResources(const SkDraw& draw, const SkPaint& paint) {
    Resources resources(paint);

    // FIXME: this is a weak heuristic and we end up with LOTS of redundant clips.
    bool hasClip   = !draw.fClipStack->isWideOpen();
    bool hasShader = SkToBool(paint.getShader());

    if (hasClip || hasShader) {
        AutoElement defs("defs", fWriter);

        if (hasClip) {
            this->addClipResources(draw, &resources);
        }

        if (hasShader) {
            this->addShaderResources(paint, &resources);
        }
    }

    return resources;
}

void SkSVGDevice::AutoElement::addShaderResources(const SkPaint& paint, Resources* resources) {
    const SkShader* shader = paint.getShader();
    SkASSERT(SkToBool(shader));

    SkShader::GradientInfo grInfo;
    grInfo.fColorCount = 0;
    if (SkShader::kLinear_GradientType != shader->asAGradient(&grInfo)) {
        // TODO: non-linear gradient support
        SkDebugf("unsupported shader type\n");
        return;
    }

    SkAutoSTArray<16, SkColor>  grColors(grInfo.fColorCount);
    SkAutoSTArray<16, SkScalar> grOffsets(grInfo.fColorCount);
    grInfo.fColors = grColors.get();
    grInfo.fColorOffsets = grOffsets.get();

    // One more call to get the actual colors/offsets.
    shader->asAGradient(&grInfo);
    SkASSERT(grInfo.fColorCount <= grColors.count());
    SkASSERT(grInfo.fColorCount <= grOffsets.count());

    resources->fPaintServer.printf("url(#%s)", addLinearGradientDef(grInfo, shader).c_str());
}

void SkSVGDevice::AutoElement::addClipResources(const SkDraw& draw, Resources* resources) {
    SkASSERT(!draw.fClipStack->isWideOpen());

    SkPath clipPath;
    (void) draw.fClipStack->asPath(&clipPath);

    SkString clipID = fResourceBucket->addClip();
    const char* clipRule = clipPath.getFillType() == SkPath::kEvenOdd_FillType ?
                           "evenodd" : "nonzero";
    {
        // clipPath is in device space, but since we're only pushing transform attributes
        // to the leaf nodes, so are all our elements => SVG userSpaceOnUse == device space.
        AutoElement clipPathElement("clipPath", fWriter);
        clipPathElement.addAttribute("id", clipID);

        SkRect clipRect = SkRect::MakeEmpty();
        if (clipPath.isEmpty() || clipPath.isRect(&clipRect)) {
            AutoElement rectElement("rect", fWriter);
            rectElement.addRectAttributes(clipRect);
            rectElement.addAttribute("clip-rule", clipRule);
        } else {
            AutoElement pathElement("path", fWriter);
            pathElement.addPathAttributes(clipPath);
            pathElement.addAttribute("clip-rule", clipRule);
        }
    }

    resources->fClip.printf("url(#%s)", clipID.c_str());
}

SkString SkSVGDevice::AutoElement::addLinearGradientDef(const SkShader::GradientInfo& info,
                                                        const SkShader* shader) {
    SkASSERT(fResourceBucket);
    SkString id = fResourceBucket->addLinearGradient();

    {
        AutoElement gradient("linearGradient", fWriter);

        gradient.addAttribute("id", id);
        gradient.addAttribute("gradientUnits", "userSpaceOnUse");
        gradient.addAttribute("x1", info.fPoint[0].x());
        gradient.addAttribute("y1", info.fPoint[0].y());
        gradient.addAttribute("x2", info.fPoint[1].x());
        gradient.addAttribute("y2", info.fPoint[1].y());

        if (!shader->getLocalMatrix().isIdentity()) {
            this->addAttribute("gradientTransform", svg_transform(shader->getLocalMatrix()));
        }

        SkASSERT(info.fColorCount >= 2);
        for (int i = 0; i < info.fColorCount; ++i) {
            SkColor color = info.fColors[i];
            SkString colorStr(svg_color(color));

            {
                AutoElement stop("stop", fWriter);
                stop.addAttribute("offset", info.fColorOffsets[i]);
                stop.addAttribute("stop-color", colorStr.c_str());

                if (SK_AlphaOPAQUE != SkColorGetA(color)) {
                    stop.addAttribute("stop-opacity", svg_opacity(color));
                }
            }
        }
    }

    return id;
}

void SkSVGDevice::AutoElement::addRectAttributes(const SkRect& rect) {
    // x, y default to 0
    if (rect.x() != 0) {
        this->addAttribute("x", rect.x());
    }
    if (rect.y() != 0) {
        this->addAttribute("y", rect.y());
    }

    this->addAttribute("width", rect.width());
    this->addAttribute("height", rect.height());
}

void SkSVGDevice::AutoElement::addPathAttributes(const SkPath& path) {
    SkString pathData;
    SkParsePath::ToSVGString(path, &pathData);
    this->addAttribute("d", pathData);
}

void SkSVGDevice::AutoElement::addTextAttributes(const SkPaint& paint) {
    this->addAttribute("font-size", paint.getTextSize());

    if (const char* textAlign = svg_text_align(paint.getTextAlign())) {
        this->addAttribute("text-anchor", textAlign);
    }

    SkString familyName;
    SkTHashSet<SkString, hash_family_string> familySet;
    SkAutoTUnref<const SkTypeface> tface(paint.getTypeface() ?
        SkRef(paint.getTypeface()) : SkTypeface::RefDefault());

    SkASSERT(tface);
    SkTypeface::Style style = tface->style();
    if (style & SkTypeface::kItalic) {
        this->addAttribute("font-style", "italic");
    }
    if (style & SkTypeface::kBold) {
        this->addAttribute("font-weight", "bold");
    }

    SkAutoTUnref<SkTypeface::LocalizedStrings> familyNameIter(tface->createFamilyNameIterator());
    SkTypeface::LocalizedString familyString;
    while (familyNameIter->next(&familyString)) {
        if (familySet.contains(familyString.fString)) {
            continue;
        }
        familySet.add(familyString.fString);
        familyName.appendf((familyName.isEmpty() ? "%s" : ", %s"), familyString.fString.c_str());
    }

    if (!familyName.isEmpty()) {
        this->addAttribute("font-family", familyName);
    }
}

SkBaseDevice* SkSVGDevice::Create(const SkISize& size, SkXMLWriter* writer) {
    if (!writer) {
        return NULL;
    }

    return SkNEW_ARGS(SkSVGDevice, (size, writer));
}

SkSVGDevice::SkSVGDevice(const SkISize& size, SkXMLWriter* writer)
    : fWriter(writer)
    , fResourceBucket(SkNEW(ResourceBucket)) {
    SkASSERT(writer);

    fLegacyBitmap.setInfo(SkImageInfo::MakeUnknown(size.width(), size.height()));

    fWriter->writeHeader();

    // The root <svg> tag gets closed by the destructor.
    fRootElement.reset(SkNEW_ARGS(AutoElement, ("svg", fWriter)));

    fRootElement->addAttribute("xmlns", "http://www.w3.org/2000/svg");
    fRootElement->addAttribute("xmlns:xlink", "http://www.w3.org/1999/xlink");
    fRootElement->addAttribute("width", size.width());
    fRootElement->addAttribute("height", size.height());
}

SkSVGDevice::~SkSVGDevice() {
}

SkImageInfo SkSVGDevice::imageInfo() const {
    return fLegacyBitmap.info();
}

const SkBitmap& SkSVGDevice::onAccessBitmap() {
    return fLegacyBitmap;
}

void SkSVGDevice::drawPaint(const SkDraw& draw, const SkPaint& paint) {
    AutoElement rect("rect", fWriter, fResourceBucket, draw, paint);
    rect.addRectAttributes(SkRect::MakeWH(SkIntToScalar(this->width()),
                                          SkIntToScalar(this->height())));
}

void SkSVGDevice::drawPoints(const SkDraw&, SkCanvas::PointMode mode, size_t count,
                             const SkPoint[], const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawPoints()\n");
}

void SkSVGDevice::drawRect(const SkDraw& draw, const SkRect& r, const SkPaint& paint) {
    AutoElement rect("rect", fWriter, fResourceBucket, draw, paint);
    rect.addRectAttributes(r);
}

void SkSVGDevice::drawOval(const SkDraw& draw, const SkRect& oval, const SkPaint& paint) {
    AutoElement ellipse("ellipse", fWriter, fResourceBucket, draw, paint);
    ellipse.addAttribute("cx", oval.centerX());
    ellipse.addAttribute("cy", oval.centerY());
    ellipse.addAttribute("rx", oval.width() / 2);
    ellipse.addAttribute("ry", oval.height() / 2);
}

void SkSVGDevice::drawRRect(const SkDraw&, const SkRRect& rr, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawRRect()\n");
}

void SkSVGDevice::drawPath(const SkDraw& draw, const SkPath& path, const SkPaint& paint,
                           const SkMatrix* prePathMatrix, bool pathIsMutable) {
    AutoElement elem("path", fWriter, fResourceBucket, draw, paint);
    elem.addPathAttributes(path);
}

void SkSVGDevice::drawBitmap(const SkDraw&, const SkBitmap& bitmap,
                             const SkMatrix& matrix, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawBitmap()\n");
}

void SkSVGDevice::drawSprite(const SkDraw&, const SkBitmap& bitmap,
                             int x, int y, const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawSprite()\n");
}

void SkSVGDevice::drawBitmapRect(const SkDraw&, const SkBitmap&, const SkRect* srcOrNull,
                                 const SkRect& dst, const SkPaint& paint,
                                 SkCanvas::DrawBitmapRectFlags flags) {
    // todo
    SkDebugf("unsupported operation: drawBitmapRect()\n");
}

void SkSVGDevice::drawText(const SkDraw& draw, const void* text, size_t len,
                           SkScalar x, SkScalar y, const SkPaint& paint) {
    AutoElement elem("text", fWriter, fResourceBucket, draw, paint);
    elem.addTextAttributes(paint);

    SVGTextBuilder builder(text, len, paint, SkPoint::Make(x, y), 0);
    elem.addAttribute("x", builder.posX());
    elem.addAttribute("y", builder.posY());
    elem.addText(builder.text());
}

void SkSVGDevice::drawPosText(const SkDraw& draw, const void* text, size_t len,
                              const SkScalar pos[], int scalarsPerPos, const SkPoint& offset,
                              const SkPaint& paint) {
    SkASSERT(scalarsPerPos == 1 || scalarsPerPos == 2);

    AutoElement elem("text", fWriter, fResourceBucket, draw, paint);
    elem.addTextAttributes(paint);

    SVGTextBuilder builder(text, len, paint, offset, scalarsPerPos, pos);
    elem.addAttribute("x", builder.posX());
    elem.addAttribute("y", builder.posY());
    elem.addText(builder.text());
}

void SkSVGDevice::drawTextOnPath(const SkDraw&, const void* text, size_t len, const SkPath& path,
                                 const SkMatrix* matrix, const SkPaint& paint) {
    SkString pathID = fResourceBucket->addPath();

    {
        AutoElement defs("defs", fWriter);
        AutoElement pathElement("path", fWriter);
        pathElement.addAttribute("id", pathID);
        pathElement.addPathAttributes(path);

    }

    {
        AutoElement textElement("text", fWriter);
        textElement.addTextAttributes(paint);

        if (matrix && !matrix->isIdentity()) {
            textElement.addAttribute("transform", svg_transform(*matrix));
        }

        {
            AutoElement textPathElement("textPath", fWriter);
            textPathElement.addAttribute("xlink:href", SkStringPrintf("#%s", pathID.c_str()));

            if (paint.getTextAlign() != SkPaint::kLeft_Align) {
                SkASSERT(paint.getTextAlign() == SkPaint::kCenter_Align ||
                         paint.getTextAlign() == SkPaint::kRight_Align);
                textPathElement.addAttribute("startOffset",
                    paint.getTextAlign() == SkPaint::kCenter_Align ? "50%" : "100%");
            }

            SVGTextBuilder builder(text, len, paint, SkPoint::Make(0, 0), 0);
            textPathElement.addText(builder.text());
        }
    }
}

void SkSVGDevice::drawVertices(const SkDraw&, SkCanvas::VertexMode, int vertexCount,
                               const SkPoint verts[], const SkPoint texs[],
                               const SkColor colors[], SkXfermode* xmode,
                               const uint16_t indices[], int indexCount,
                               const SkPaint& paint) {
    // todo
    SkDebugf("unsupported operation: drawVertices()\n");
}

void SkSVGDevice::drawDevice(const SkDraw&, SkBaseDevice*, int x, int y,
                             const SkPaint&) {
    // todo
    SkDebugf("unsupported operation: drawDevice()\n");
}
