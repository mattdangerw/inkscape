#ifndef __NR_FILTER_GAUSSIAN_H__
#define __NR_FILTER_GAUSSIAN_H__

/*
 * Gaussian blur renderer
 *
 * Authors:
 *   Niko Kiirala <niko@kiirala.com>
 *   bulia byak
 *   Jasper van de Gronde <th.v.d.gronde@hccnet.nl>
 *
 * Copyright (C) 2006 authors
 *
 * Released under GNU GPL, read the file 'COPYING' for more information
 */

#include "display/nr-filter-primitive.h"
#include "display/nr-filter-slot.h"
#include "display/nr-filter-units.h"
#include "libnr/nr-pixblock.h"
#include <2geom/forward.h>
#include "libnr/nr-rect-l.h"

enum {
    BLUR_QUALITY_BEST = 2,
    BLUR_QUALITY_BETTER = 1,
    BLUR_QUALITY_NORMAL = 0,
    BLUR_QUALITY_WORSE = -1,
    BLUR_QUALITY_WORST = -2
};

namespace Inkscape {
namespace Filters {

class FilterGaussian : public FilterPrimitive {
public:
    FilterGaussian();
    static FilterPrimitive *create();
    virtual ~FilterGaussian();

    virtual int render(FilterSlot &slot, FilterUnits const &units);
    virtual void area_enlarge(NRRectL &area, Geom::Affine const &m);
    virtual FilterTraits get_input_traits();

    /**
     * Set the standard deviation value for gaussian blur. Deviation along
     * both axis is set to the provided value.
     * Negative value, NaN and infinity are considered an error and no
     * changes to filter state are made. If not set, default value of zero
     * is used, which means the filter results in transparent black image.
     */
    void set_deviation(double deviation);
    /**
     * Set the standard deviation value for gaussian blur. First parameter
     * sets the deviation alogn x-axis, second along y-axis.
     * Negative value, NaN and infinity are considered an error and no
     * changes to filter state are made. If not set, default value of zero
     * is used, which means the filter results in transparent black image.
     */
    void set_deviation(double x, double y);

private:
    double _deviation_x;
    double _deviation_y;
};


} /* namespace Filters */
} /* namespace Inkscape */




#endif /* __NR_FILTER_GAUSSIAN_H__ */
/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
