// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2018 Intel Corporation


#include <iomanip>   // hex, dec (debug)

#include "opencv2/gapi/own/convert.hpp"

#include "opencv2/gapi/fluid/gfluidbuffer.hpp"
#include "backends/fluid/gfluidbuffer_priv.hpp"
#include "opencv2/gapi/opencv_includes.hpp"

#include "backends/fluid/gfluidutils.hpp" // saturate

namespace cv {
namespace gapi {

namespace fluid {
bool operator == (const fluid::Border& b1, const fluid::Border& b2)
{
    return b1.type == b2.type && b1.value == b2.value;
}
} // namespace fluid

// Fluid BorderHandler implementation /////////////////////////////////////////////////

namespace {
template<typename T>
// Expected inputs:
// row - row buffer allocated with border in mind (have memory for both image and border pixels)
// length - size of the buffer with left and right borders included
void fillBorderReplicateRow(uint8_t* row, int length, int chan, int borderSize)
{
    auto leftBorder  = reinterpret_cast<T*>(row);
    auto rightBorder = leftBorder + (length - borderSize) * chan;
    for (int b = 0; b < borderSize; b++)
    {
        for (int c = 0; c < chan; c++)
        {
            leftBorder [b*chan + c] = leftBorder [borderSize*chan + c];
            rightBorder[b*chan + c] = rightBorder[-chan + c];
        }
    }
}

template<typename T>
void fillBorderReflectRow(uint8_t* row, int length, int chan, int borderSize)
{
    auto leftBorder  = reinterpret_cast<T*>(row);
    auto rightBorder = leftBorder + (length - borderSize) * chan;
    for (int b = 0; b < borderSize; b++)
    {
        for (int c = 0; c < chan; c++)
        {
            leftBorder [b*chan + c] = leftBorder [(2*borderSize - b)*chan + c];
            rightBorder[b*chan + c] = rightBorder[(-b - 2)*chan + c];
        }
    }
}

template<typename T>
void fillConstBorderRow(uint8_t* row, int length, int chan, int borderSize, cv::gapi::own::Scalar borderValue)
{
    GAPI_DbgAssert(chan > 0 && chan <= 4);

    auto leftBorder  = reinterpret_cast<T*>(row);
    auto rightBorder = leftBorder + (length - borderSize) * chan;
    for (int b = 0; b < borderSize; b++)
    {
        for (int c = 0; c < chan; c++)
        {
            leftBorder [b*chan + c] = fluid::saturate<T>(borderValue[c], fluid::roundd);
            rightBorder[b*chan + c] = fluid::saturate<T>(borderValue[c], fluid::roundd);
        }
    }
}

// Fills const border pixels in the whole mat
void fillBorderConstant(int borderSize, cv::gapi::own::Scalar borderValue, cv::Mat& mat)
{
    // cv::Scalar can contain maximum 4 chan
    GAPI_Assert(mat.channels() > 0 && mat.channels() <= 4);

    auto getFillBorderRowFunc = [&](int type) {
        switch(type)
        {
        case CV_8U:  return &fillConstBorderRow< uint8_t>; break;
        case CV_16S: return &fillConstBorderRow< int16_t>; break;
        case CV_16U: return &fillConstBorderRow<uint16_t>; break;
        case CV_32F: return &fillConstBorderRow< float  >; break;
        default: CV_Assert(false); return &fillConstBorderRow<uint8_t>;
        }
    };

    auto fillBorderRow = getFillBorderRowFunc(mat.depth());
    for (int y = 0; y < mat.rows; y++)
    {
        fillBorderRow(mat.ptr(y), mat.cols, mat.channels(), borderSize, borderValue);
    }
}
} // anonymous namespace

fluid::BorderHandler::BorderHandler(int border_size)
{
    CV_Assert(border_size > 0);
    m_border_size = border_size;
}

template <int BorderType>
fluid::BorderHandlerT<BorderType>::BorderHandlerT(int border_size, int data_type)
    : BorderHandler(border_size)
{
    auto getFillBorderRowFunc = [&](int border, int dataType) {
        if (border == cv::BORDER_REPLICATE)
        {
            switch(dataType)
            {
            case CV_8U:  return &fillBorderReplicateRow< uint8_t>; break;
            case CV_16S: return &fillBorderReplicateRow< int16_t>; break;
            case CV_16U: return &fillBorderReplicateRow<uint16_t>; break;
            case CV_32F: return &fillBorderReplicateRow< float  >; break;
            default: CV_Assert(!"Unsupported data type"); return &fillBorderReplicateRow<uint8_t>;
            }
        }
        else if (border == cv::BORDER_REFLECT_101)
        {
            switch(dataType)
            {
            case CV_8U:  return &fillBorderReflectRow< uint8_t>; break;
            case CV_16S: return &fillBorderReflectRow< int16_t>; break;
            case CV_16U: return &fillBorderReflectRow<uint16_t>; break;
            case CV_32F: return &fillBorderReflectRow< float  >; break;
            default: CV_Assert(!"Unsupported data type"); return &fillBorderReflectRow<uint8_t>;
            }
        }
        else
        {
            CV_Assert(!"Unsupported border type");
            return &fillBorderReflectRow<uint8_t>;
        }
    };

    m_fill_border_row = getFillBorderRowFunc(BorderType, data_type);
}

namespace {
template <int BorderType> int getBorderIdx(int log_idx, int desc_height);

template<> int getBorderIdx<cv::BORDER_REPLICATE>(int log_idx, int desc_height)
{
    return log_idx < 0 ? 0 : desc_height - 1;
}

template<> int getBorderIdx<cv::BORDER_REFLECT_101>(int log_idx, int desc_height)
{
    return log_idx < 0 ? -log_idx : 2*(desc_height - 1) - log_idx;
}
} // namespace

template <int BorderType>
const uint8_t* fluid::BorderHandlerT<BorderType>::inLineB(int log_idx, const BufferStorageWithBorder& data, int desc_height) const
{
    auto idx = getBorderIdx<BorderType>(log_idx, desc_height);
    return data.ptr(idx);
}

fluid::BorderHandlerT<cv::BORDER_CONSTANT>::BorderHandlerT(int border_size, cv::gapi::own::Scalar border_value, int data_type, int desc_width)
    : BorderHandler(border_size), m_border_value(border_value)
{
    m_const_border.create(1, desc_width + 2*m_border_size, data_type);
    m_const_border = cv::gapi::own::to_ocv(border_value);
}

const uint8_t* fluid::BorderHandlerT<cv::BORDER_CONSTANT>::inLineB(int /*log_idx*/, const BufferStorageWithBorder& /*data*/, int /*desc_height*/) const
{
    return m_const_border.ptr(0, m_border_size);
}

void fluid::BorderHandlerT<cv::BORDER_CONSTANT>::fillCompileTimeBorder(BufferStorageWithBorder& data) const
{
    cv::gapi::fillBorderConstant(m_border_size, m_border_value, data.data());
}

template <int BorderType>
void fluid::BorderHandlerT<BorderType>::updateBorderPixels(BufferStorageWithBorder &data, int startLine, int nLines) const
{
    auto& mat   = data.data();
    auto length = mat.cols;
    auto chan   = mat.channels();

    for (int l = startLine; l < startLine + nLines; l++)
    {
        auto row = mat.ptr(data.physIdx(l));
        m_fill_border_row(row, length, chan, m_border_size);
    }
}

std::size_t fluid::BorderHandlerT<cv::BORDER_CONSTANT>::size() const
{
    return m_const_border.total() * m_const_border.elemSize();
}

// Fluid BufferStorage implementation //////////////////////////////////////////
void fluid::BufferStorageWithBorder::create(int capacity, int desc_width, int dtype, int border_size, Border border)
{
    auto width = (desc_width + 2*border_size);
    m_data.create(capacity, width, dtype);

    switch(border.type)
    {
    case cv::BORDER_CONSTANT:
        m_borderHandler.reset(new BorderHandlerT<cv::BORDER_CONSTANT>(border_size, border.value, dtype, desc_width)); break;
    case cv::BORDER_REPLICATE:
        m_borderHandler.reset(new BorderHandlerT<cv::BORDER_REPLICATE>(border_size, dtype)); break;
    case cv::BORDER_REFLECT_101:
        m_borderHandler.reset(new BorderHandlerT<cv::BORDER_REFLECT_101>(border_size, dtype)); break;
    default:
        CV_Assert(false);
    }

    m_borderHandler->fillCompileTimeBorder(*this);
}

void fluid::BufferStorageWithoutBorder::create(int capacity, int desc_width, int dtype)
{
    auto width = desc_width;
    m_data.create(capacity, width, dtype);

    m_is_virtual = true;
}

const uint8_t* fluid::BufferStorageWithBorder::inLineB(int log_idx, int desc_height) const
{
    if (log_idx < 0 || log_idx >= desc_height)
    {
        return m_borderHandler->inLineB(log_idx, *this, desc_height);
    }
    else
    {
        return ptr(log_idx);
    }
}

const uint8_t* fluid::BufferStorageWithoutBorder::inLineB(int log_idx, int /*desc_height*/) const
{
    return ptr(log_idx);
}

static void copyWithoutBorder(const cv::Mat& src, int src_border_size, cv::Mat& dst, int dst_border_size, int startSrcLine, int startDstLine, int lpi)
{
    // FIXME use cv::gapi::own::Rect when implement cv::gapi::own::Mat
    auto subSrc = src(cv::Rect{src_border_size, startSrcLine, src.cols - 2*src_border_size, lpi});
    auto subDst = dst(cv::Rect{dst_border_size, startDstLine, dst.cols - 2*dst_border_size, lpi});

    subSrc.copyTo(subDst);
}

void fluid::BufferStorageWithoutBorder::copyTo(BufferStorageWithBorder &dst, int startLine, int nLines) const
{
    for (int l = startLine; l < startLine + nLines; l++)
    {
        copyWithoutBorder(m_data, 0, dst.data(), dst.borderSize(), physIdx(l), dst.physIdx(l), 1);
    }
}

void fluid::BufferStorageWithBorder::copyTo(BufferStorageWithBorder &dst, int startLine, int nLines) const
{
    // Copy required lpi lines line by line (to avoid wrap if invoked for multiple lines)
    for (int l = startLine; l < startLine + nLines; l++)
    {
        copyWithoutBorder(m_data, borderSize(), dst.data(), dst.borderSize(), physIdx(l), dst.physIdx(l), 1);
    }
}

// FIXME? remember parent and remove src parameter?
void fluid::BufferStorageWithBorder::updateBeforeRead(int startLine, int nLines, const BufferStorage& src)
{
    // TODO:
    // Cover with tests!!
    // (Ensure that there are no redundant copies done
    // and only required (not fetched before) lines are copied)

    GAPI_DbgAssert(startLine >= 0);

    src.copyTo(*this, startLine, nLines);
    m_borderHandler->updateBorderPixels(*this, startLine, nLines);
}

void fluid::BufferStorageWithoutBorder::updateBeforeRead(int /*startLine*/, int /*lpi*/, const BufferStorage& /*src*/)
{
    /* nothing */
}

void fluid::BufferStorageWithBorder::updateAfterWrite(int startLine, int nLines)
{
    // FIXME?
    // Actually startLine + nLines can be > logical height so
    // redundant end lines which will never be read
    // can be filled in the ring buffer
    m_borderHandler->updateBorderPixels(*this, startLine, nLines);
}

void fluid::BufferStorageWithoutBorder::updateAfterWrite(int /*startLine*/, int /*lpi*/)
{
    /* nothing */
}

size_t fluid::BufferStorageWithBorder::size() const
{
    return m_data.total()*m_data.elemSize() + m_borderHandler->size();
}

size_t fluid::BufferStorageWithoutBorder::size() const
{
    return m_data.total()*m_data.elemSize();
}

namespace fluid {
namespace {
std::unique_ptr<fluid::BufferStorage> createStorage(int capacity, int desc_width, int type,
                                                    int border_size, fluid::BorderOpt border);
std::unique_ptr<fluid::BufferStorage> createStorage(int capacity, int desc_width, int type,
                                                    int border_size, fluid::BorderOpt border)
{
    if (border)
    {
        std::unique_ptr<fluid::BufferStorageWithBorder> storage(new BufferStorageWithBorder);
        storage->create(capacity, desc_width, type, border_size, border.value());
        return std::move(storage);
    }

    std::unique_ptr<BufferStorageWithoutBorder> storage(new BufferStorageWithoutBorder);
    storage->create(capacity, desc_width, type);
    return std::move(storage);
}

std::unique_ptr<BufferStorage> createStorage(const cv::Mat& data, cv::gapi::own::Rect roi);
std::unique_ptr<BufferStorage> createStorage(const cv::Mat& data, cv::gapi::own::Rect roi)
{
    std::unique_ptr<BufferStorageWithoutBorder> storage(new BufferStorageWithoutBorder);
    storage->attach(data, roi);
    return std::move(storage);
}
} // namespace
} // namespace fluid

// Fluid View implementation ///////////////////////////////////////////////////

void fluid::View::Priv::reset(int linesForFirstIteration)
{
    GAPI_DbgAssert(m_p);

    m_lines_next_iter = linesForFirstIteration;
    m_read_caret = m_p->priv().readStart();
}

void fluid::View::Priv::readDone(int linesRead, int linesForNextIteration)
{
    CV_DbgAssert(m_p);
    m_read_caret += linesRead;
    m_read_caret %= m_p->meta().size.height;
    m_lines_next_iter = linesForNextIteration;
}

bool fluid::View::Priv::ready() const
{
    auto lastWrittenLine = m_p->priv().writeStart() + m_p->linesReady();
    // + bottom border
    if (lastWrittenLine == m_p->meta().size.height) lastWrittenLine += m_border_size;
    // + top border
    lastWrittenLine += m_border_size;

    auto lastRequiredLine = m_read_caret + m_lines_next_iter;

    return lastWrittenLine >= lastRequiredLine;
}

fluid::ViewPrivWithoutOwnBorder::ViewPrivWithoutOwnBorder(const Buffer *parent, int borderSize)
{
    CV_Assert(parent);
    m_p           = parent;
    m_border_size = borderSize;
}

const uint8_t* fluid::ViewPrivWithoutOwnBorder::InLineB(int index) const
{
    GAPI_DbgAssert(m_p);

    const auto &p_priv = m_p->priv();

    CV_Assert(   index >= -m_border_size
              && index <  -m_border_size + m_lines_next_iter);

    const int log_idx = m_read_caret + index;

    return p_priv.storage().inLineB(log_idx, m_p->meta().size.height);
}

fluid::ViewPrivWithOwnBorder::ViewPrivWithOwnBorder(const Buffer *parent, int lineConsumption, int borderSize, Border border)
{
    GAPI_Assert(parent);
    m_p           = parent;
    m_border_size = borderSize;

    auto desc = m_p->meta();
    int  type = CV_MAKETYPE(desc.depth, desc.chan);
    m_own_storage.create(lineConsumption, desc.size.width, type, borderSize, border);
}

void fluid::ViewPrivWithOwnBorder::prepareToRead()
{
    int startLine = 0;
    int nLines = 0;

    if (m_read_caret == m_p->priv().readStart())
    {
        // Need to fetch full window on the first iteration
        startLine = (m_read_caret > m_border_size) ? m_read_caret - m_border_size : 0;
        nLines = m_lines_next_iter;
    }
    else
    {
        startLine = m_read_caret + m_border_size;
        nLines = m_lines_next_iter - 2*m_border_size;
    }

    m_own_storage.updateBeforeRead(startLine, nLines, m_p->priv().storage());
}

std::size_t fluid::ViewPrivWithOwnBorder::size() const
{
    GAPI_DbgAssert(m_p);
    return m_own_storage.size();
}

const uint8_t* fluid::ViewPrivWithOwnBorder::InLineB(int index) const
{
    GAPI_DbgAssert(m_p);

    GAPI_Assert( index >= -m_border_size
              && index <  -m_border_size + m_lines_next_iter);

    const int log_idx = m_read_caret + index;

    return m_own_storage.inLineB(log_idx, m_p->meta().size.height);
}

const uint8_t* fluid::View::InLineB(int index) const
{
    return m_priv->InLineB(index);
}

fluid::View::operator bool() const
{
    return m_priv != nullptr && m_priv->m_p != nullptr;
}

int fluid::View::length() const
{
    return m_priv->m_p->length();
}

bool fluid::View::ready() const
{
    return m_priv->ready();
}

int fluid::View::y() const
{
    return m_priv->m_read_caret - m_priv->m_border_size;
}

cv::GMatDesc fluid::View::meta() const
{
    // FIXME: cover with test!
    return m_priv->m_p->meta();
}

fluid::View::Priv& fluid::View::priv()
{
    return *m_priv;
}

const fluid::View::Priv& fluid::View::priv() const
{
    return *m_priv;
}

// Fluid Buffer implementation /////////////////////////////////////////////////

fluid::Buffer::Priv::Priv(int read_start, cv::gapi::own::Rect roi)
    : m_readStart(read_start)
    , m_roi(roi)
{}

void fluid::Buffer::Priv::init(const cv::GMatDesc &desc,
                               int line_consumption,
                               int border_size,
                               int skew,
                               int wlpi,
                               int readStartPos,
                               cv::gapi::own::Rect roi)
{
    GAPI_Assert(m_line_consumption == -1);
    GAPI_Assert(line_consumption > 0);

    m_line_consumption = line_consumption;
    m_border_size      = border_size;
    m_skew             = skew;
    m_writer_lpi       = wlpi;
    m_desc             = desc;
    m_readStart        = readStartPos;
    m_roi              = roi;
}

void fluid::Buffer::Priv::allocate(BorderOpt border)
{
    GAPI_Assert(!m_storage);

    // Init physical buffer

    // FIXME? combine with skew?
    auto maxRead    = m_line_consumption + m_skew;
    auto maxWritten = m_writer_lpi;

    auto max = std::max(maxRead, maxWritten);
    auto min = std::min(maxRead, maxWritten);

    // FIXME:
    // Fix the deadlock (completely)!!!
    auto data_height = static_cast<int>(std::ceil((double)max / min) * min);

    m_storage = createStorage(data_height,
                              m_desc.size.width,
                              CV_MAKETYPE(m_desc.depth, m_desc.chan),
                              m_border_size,
                              border);

    // Finally, initialize carets
    m_write_caret = 0;
}

void fluid::Buffer::Priv::bindTo(const cv::Mat &data, bool is_input)
{
    // FIXME: move all these fields into a separate structure
    GAPI_Assert(m_skew == 0);
    GAPI_Assert(m_desc == cv::descr_of(data));
    if ( is_input) CV_Assert(m_writer_lpi  == 1);

    m_storage = createStorage(data, m_roi);

    m_is_input    = is_input;
    m_write_caret = is_input ? writeEnd(): writeStart();
    // NB: views remain the same!
}

bool fluid::Buffer::Priv::full() const
{
    int slowest_y = writeEnd();
    if (!m_views.empty())
    {
        // reset with maximum possible value and then find minimum
        slowest_y = m_desc.size.height;
        for (const auto &v : m_views) slowest_y = std::min(slowest_y, v.y());
    }

    return m_write_caret + lpi() - slowest_y > m_storage->rows();
}

void fluid::Buffer::Priv::writeDone()
{
    // There are possible optimizations which can be done to fill a border values
    // in compile time of the graph (for example border is const),
    // so there is no need to update border values after each write.
    // If such optimizations weren't applied, fill border for lines
    // which have been just written
    m_storage->updateAfterWrite(m_write_caret, m_writer_lpi);

    // Final write may produce less LPI, so
    // write caret may exceed logical buffer size
    m_write_caret += m_writer_lpi;
    // FIXME: add consistency check!
}

void fluid::Buffer::Priv::reset()
{
    m_write_caret = m_is_input ? writeEnd() : writeStart();
}

int fluid::Buffer::Priv::size() const
{
    std::size_t view_sz = 0;
    for (const auto &v : m_views) view_sz += v.priv().size();

    auto total = view_sz;
    if (m_storage) total += m_storage->size();

    // FIXME: Change API to return size_t!!!
    return static_cast<int>(total);
}

int fluid::Buffer::Priv::linesReady() const
{
    if (m_is_input)
    {
        return m_storage->rows();
    }
    else
    {
        const int writes = std::min(m_write_caret - writeStart(), outputLines());
        return writes;
    }
}

uint8_t* fluid::Buffer::Priv::OutLineB(int index)
{
    GAPI_Assert(index >= 0 && index < m_writer_lpi);

    return m_storage->ptr(m_write_caret + index);
}

int fluid::Buffer::Priv::lpi() const
{
    // FIXME:
    // m_write_caret can be greater than m_writeRoi.y + m_writeRoi.height, so return value can be negative !!!
    return std::min(writeEnd() - m_write_caret, m_writer_lpi);
}

fluid::Buffer::Buffer()
    : m_priv(new Priv())
{
}

fluid::Buffer::Buffer(const cv::GMatDesc &desc)
    : m_priv(new Priv())
{
    int lineConsumption = 1;
    int border = 0, skew = 0, wlpi = 1, readStart = 0;
    cv::gapi::own::Rect roi = {0, 0, desc.size.width, desc.size.height};
    m_priv->init(desc, lineConsumption, border, skew, wlpi, readStart, roi);
    m_priv->allocate({});
}

fluid::Buffer::Buffer(const cv::GMatDesc &desc,
                      int max_line_consumption,
                      int border_size,
                      int skew,
                      int wlpi,
                      BorderOpt border)
    : m_priv(new Priv())
{
    int readStart = 0;
    cv::gapi::own::Rect roi = {0, 0, desc.size.width, desc.size.height};
    m_priv->init(desc, max_line_consumption, border_size, skew, wlpi, readStart, roi);
    m_priv->allocate(border);
}

fluid::Buffer::Buffer(const cv::Mat &data, bool is_input)
    : m_priv(new Priv())
{
    int lineConsumption = 1;
    int border = 0, skew = 0, wlpi = 1, readStart = 0;
    cv::gapi::own::Rect roi{0, 0, data.cols, data.rows};
    m_priv->init(descr_of(data), lineConsumption, border, skew, wlpi, readStart, roi);
    m_priv->bindTo(data, is_input);
}

uint8_t* fluid::Buffer::Buffer::OutLineB(int index)
{
    return m_priv->OutLineB(index);
}

int fluid::Buffer::linesReady() const
{
    return m_priv->linesReady();
}

int fluid::Buffer::length() const
{
    return meta().size.width;
}

int fluid::Buffer::lpi() const
{
    return m_priv->lpi();
}

cv::GMatDesc fluid::Buffer::meta() const
{
    return m_priv->meta();
}

fluid::View::View(Priv* p)
    : m_priv(p)
{ /* nothing */ }

fluid::View fluid::Buffer::mkView(int lineConsumption, int borderSize, BorderOpt border, bool ownStorage)
{
    // FIXME: logic outside of Priv (because View takes pointer to Buffer)
    auto view = ownStorage ? View(new ViewPrivWithOwnBorder(this, lineConsumption, borderSize, border.value()))
                           : View(new ViewPrivWithoutOwnBorder(this, borderSize));
    m_priv->addView(view);
    return view;
}

void fluid::debugBufferPriv(const fluid::Buffer& buffer, std::ostream &os)
{
    // FIXME Use cv::gapi::own Size and Rect with operator<<, when merged ADE-285
    const auto& p = buffer.priv();
    os << "Fluid buffer " << std::hex << &buffer << std::dec
       << " " << p.m_desc.size.width << " x " << p.m_desc.size.height << "]"
       << " readStart:" << p.m_readStart
       << " roi:" << "[" << p.m_roi.width << " x " << p.m_roi.height << " from (" << p.m_roi.x << ", " << p.m_roi.y << ")]"
       <<" (phys " << "[" << p.storage().cols() << " x " <<  p.storage().rows() << "]" << ") :"
       << "  w: " << p.m_write_caret
       << ", r: [";
    for (const auto &v : p.m_views) { os << &v.priv() << ":" << v.y() << " "; }
    os << "], avail: " << buffer.linesReady()
       << std::endl;
}

void fluid::Buffer::debug(std::ostream &os) const
{
    debugBufferPriv(*this, os);
}

fluid::Buffer::Priv& fluid::Buffer::priv()
{
    return *m_priv;
}

const fluid::Buffer::Priv& fluid::Buffer::priv() const
{
    return *m_priv;
}

int fluid::Buffer::y() const
{
    return m_priv->y();
}

} // namespace cv::gapi
} // namespace cv
