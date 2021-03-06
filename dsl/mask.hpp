//
// Copyright (c) 2012, University of Erlangen-Nuremberg
// Copyright (c) 2012, Siemens AG
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#ifndef __MASK_HPP__
#define __MASK_HPP__

#include "iterationspace.hpp"
#include "types.hpp"

namespace hipacc {
class MaskBase {
    protected:
        const int size_x, size_y;
        const int offset_x, offset_y;
        uchar *domain_space;
        IterationSpaceBase iteration_space;

    public:
        MaskBase(int size_x, int size_y) :
            size_x(size_x),
            size_y(size_y),
            offset_x(-size_x/2),
            offset_y(-size_y/2),
            domain_space(new uchar[size_x*size_y]),
            iteration_space(size_x, size_y)
        {
            assert(size_x>0 && size_y>0 && "Size for Domain must be positive!");
            // initialize full domain
            for (int i = 0; i < size_x*size_y; ++i) {
              domain_space[i] = 1;
            }
        }

        MaskBase(const MaskBase &mask) :
            size_x(mask.size_x),
            size_y(mask.size_y),
            offset_x(-mask.size_x/2),
            offset_y(-mask.size_y/2),
            domain_space(new uchar[mask.size_x*mask.size_y]),
            iteration_space(mask.size_x, mask.size_y)
        {
            for (int y=0; y<size_y; ++y) {
              for (int x=0; x<size_x; ++x) {
                domain_space[y * size_x + x] = mask.domain_space[y * size_x + x];
              }
            }
        }

        ~MaskBase() {
            if (domain_space != nullptr) {
              delete[] domain_space;
              domain_space = nullptr;
            }
        }

        int getSizeX() const { return size_x; }
        int getSizeY() const { return size_y; }

        virtual int getX() = 0;
        virtual int getY() = 0;

    friend class Domain;
    template<typename> friend class Mask;
};


class Domain : public MaskBase {
    public:
        class DomainIterator : public ElementIterator {
            private:
                uchar *domain_space;

            public:
                DomainIterator(int width=0, int height=0,
                               int offsetx=0, int offsety=0,
                               const IterationSpaceBase *iterspace=nullptr,
                               uchar *domain_space=nullptr) :
                    ElementIterator(width, height, offsetx, offsety, iterspace),
                    domain_space(domain_space)
                {
                    if (domain_space != nullptr) {
                        // set current coordinate before domain
                        coord.x = min_x-1;
                        coord.y = min_y;

                        // use increment to search first non-zero value
                        ++(*this);
                    }
                }

                ~DomainIterator() {}

                // increment so we iterate over elements in a block
                DomainIterator &operator++() {
                    do {
                        if (iteration_space) {
                            coord.x++;
                            if (coord.x >= max_x) {
                                coord.x = min_x;
                                coord.y++;
                                if (coord.y >= max_y) {
                                    iteration_space = nullptr;
                                }
                            }
                        }
                    } while (nullptr != iteration_space &&
                             (nullptr == domain_space ||
                              0 == domain_space[(coord.y-min_y) * (max_x-min_x)
                                                + (coord.x-min_x)]));
                    return *this;
                }
            };

        // Helper class: Force Domain assignment
        //   D(1, 1) = 0;
        // to be an CXXOperatorCallExpr, which is easier to track.
        class DomainSetter {
            friend class Domain;
            private:
                uchar *domain_space;
                unsigned int pos;
                DomainSetter(uchar *domain_space, unsigned int pos)
                        : domain_space(domain_space), pos(pos) {
                }

            public:
                DomainSetter &operator=(const uchar val) {
                    domain_space[pos] = val;
                    return *this;
                }
        };

    protected:
        DomainIterator *DI;

    public:
        Domain(int size_x, int size_y) :
            MaskBase(size_x, size_y),
            DI(nullptr) {}

        template <int size_y, int size_x>
        Domain(const uchar (&domain)[size_y][size_x]) :
            MaskBase(size_x, size_y),
            DI(nullptr) {}

        Domain(const MaskBase &mask) :
            MaskBase(mask),
            DI(nullptr) {}

        Domain(const Domain &domain) :
            MaskBase(domain),
            DI(domain.DI) {}

        ~Domain() {}

        int getX() {
            assert(DI && "DomainIterator for Domain not set!");
            return DI->getX();
        }
        int getY() {
            assert(DI && "DomainIterator for Domain not set!");
            return DI->getY();
        }

        DomainSetter operator()(unsigned int x, unsigned int y) {
            x += size_x >> 1;
            y += size_y >> 1;
            if ((int)x < size_x && (int)y < size_y) {
                return DomainSetter(domain_space, y * size_x + x);
            } else {
                return DomainSetter(domain_space, 0);
            }
        }

        Domain &operator=(const uchar *other) {
            for (int y=0; y<size_y; ++y) {
                for (int x=0; x<size_x; ++x) {
                    domain_space[y * size_x + x] = other[y * size_x + x];
                }
            }

            return *this;
        }

        void operator=(const Domain &dom) {
            assert(size_x==dom.getSizeX() && size_y==dom.getSizeY() &&
                    "Domain sizes must be equal.");
            for (int y=0; y<size_y; ++y) {
                for (int x=0; x<size_x; ++x) {
                    domain_space[y * size_x + x] = dom.domain_space[y * size_x + x];
                }
            }
        }

        void operator=(const MaskBase &mask) {
            assert(size_x==mask.getSizeX() && size_y==mask.getSizeY() &&
                    "Domain and Mask size must be equal.");
            for (int y=0; y<size_y; ++y) {
                for (int x=0; x<size_x; ++x) {
                    domain_space[y * size_x + x] = mask.domain_space[y * size_x + x];
                }
            }
        }

        void setDI(DomainIterator *di) { DI = di; }
        DomainIterator begin() const {
            return DomainIterator(size_x, size_y, offset_x, offset_y,
                                  &iteration_space, domain_space);
        }
        DomainIterator end() const { return DomainIterator(); }
};


template<typename data_t>
class Mask : public MaskBase {
    private:
        ElementIterator *EI;
        data_t *array;

        template <int size_y, int size_x>
        void init(const data_t (&mask)[size_y][size_x]) {
            for (int y=0; y<size_y; ++y) {
                for (int x=0; x<size_x; ++x) {
                    array[y*size_x + x] = mask[y][x];
                    // set holes in underlying domain_space
                    if (mask[y][x] == 0) {
                        domain_space[y*size_x + x] = 0;
                    } else {
                        domain_space[y*size_x + x] = 1;
                    }
                }
            }
        }


    public:
        template <int size_y, int size_x>
        Mask(const data_t (&mask)[size_y][size_x]) :
            MaskBase(size_x, size_y),
            EI(nullptr),
            array(new data_t[size_x*size_y])
        {
            init(mask);
        }

        Mask(const Mask &mask) :
            MaskBase(mask.size_x, mask.size_y),
            array(new data_t[mask.size_x*mask.size_y])
        {
            init(mask.array);
        }

        ~Mask() {
            if (array != nullptr) {
              delete[] array;
              array = nullptr;
            }
        }

        int getX() {
            assert(EI && "ElementIterator for Mask not set!");
            return EI->getX();
        }
        int getY() {
            assert(EI && "ElementIterator for Mask not set!");
            return EI->getY();
        }

        data_t &operator()(void) {
            assert(EI && "ElementIterator for Mask not set!");
            return array[(EI->getY()-offset_y)*size_x + EI->getX()-offset_x];
        }
        data_t &operator()(const int xf, const int yf) {
            return array[(yf-offset_y)*size_x + xf-offset_x];
        }
        data_t &operator()(Domain &D) {
            assert(D.getSizeX()==size_x && D.getSizeY()==size_y &&
                    "Domain and Mask size must be equal.");
            return array[(D.getY()-offset_y)*size_x + D.getX()-offset_x];
        }

        void setEI(ElementIterator *ei) { EI = ei; }
        ElementIterator begin() const {
            return ElementIterator(size_x, size_y, offset_x, offset_y,
                    &iteration_space);
        }
        ElementIterator end() const { return ElementIterator(); }
};
} // end namespace hipacc

#endif // __MASK_HPP__

