#pragma once

#include <vector>
#include <map>
#include <list>
#include <set>
#include <fstream>
#include <sstream>
#include <string>

#include "mesh_ids.h"
class Index3
{
    public:
        GlobalId x[3];

        Index3()
        {
            x[0] = 0;
            x[1] = 0;
            x[2] = 0;
        }

        Index3(GlobalId _x, GlobalId _y, GlobalId _z)
        {
            x[0] = _x;
            x[1] = _y;
            x[2] = _z;
        }

        void swapel(int a, int b)
        {
            GlobalId tmp;
            tmp = x[a];
            x[a] = x[b];
            x[b] = tmp;
        }

        void Sort()
        {
            if(x[1] < x[0]) swapel(1,0);
            if(x[2] < x[1]) swapel(2,1);
            if(x[1] < x[0]) swapel(1,0);
        }
};

//判断两个不同Index3内的三个参数是否有序
inline bool fncomp(Index3 in1, Index3 in2)
{
    if(in1.x[0] < in2.x[0])
        return true;
    else if(in1.x[0] > in2.x[0])
        return false;

    if(in1.x[1] < in2.x[1])
        return true;
    else if(in1.x[1] > in2.x[1])
        return false;

    if(in1.x[2] < in2.x[2])
        return true;
    else if(in1.x[2] > in2.x[2])
        return false;

    return false;
}

// Text layout is unchanged; every distributed ID reaches the stream as int64.
inline void write_partition_element(std::ostream &out, GlobalId element,
                                    const int tet[4], const GlobalId *points)
{
    out << element << " 1 504 " << points[tet[0]] << " " << points[tet[1]]
        << " " << points[tet[2]] << " " << points[tet[3]] << std::endl;
}

inline void write_partition_node(std::ostream &out, GlobalId point, const double xyz[3])
{
    out << point << " -1 " << xyz[0] << " " << xyz[1] << " " << xyz[2] << std::endl;
}

inline void write_partition_shared(std::ostream &out, GlobalId point, const std::string &holders)
{
    out << point << " " << holders << std::endl;
}

inline void write_partition_boundary(std::ostream &out, int boundary, int geometry,
                                     GlobalId parent, const int triangle[3], const GlobalId *points)
{
    out << boundary << " " << geometry << " " << parent << " 0 303 "
        << points[triangle[0]] << " " << points[triangle[1]] << " "
        << points[triangle[2]] << std::endl;
}
