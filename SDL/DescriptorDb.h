/******************************************************************************
 * This file is part of dirtsand.                                             *
 *                                                                            *
 * dirtsand is free software: you can redistribute it and/or modify           *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * dirtsand is distributed in the hope that it will be useful,                *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with dirtsand.  If not, see <http://www.gnu.org/licenses/>.          *
 ******************************************************************************/

#ifndef _SDL_DESCRIPTORDB_H
#define _SDL_DESCRIPTORDB_H

#include "StateInfo.h"
#include <vector>

namespace SDL
{
    struct VarDescriptor
    {
        VarType m_type;
        DS::String m_typeName;
        DS::String m_name;
        int m_size;
        SDL::Value m_default;
        DS::String m_defaultOption, m_displayOption;
    };

    struct StateDescriptor
    {
        DS::String m_name;
        int m_version;
        std::vector<VarDescriptor> m_vars;
    };

    class DescriptorDb
    {
    public:
        static bool LoadDescriptors(const char* sdlpath);

    private:
        DescriptorDb() { }
        DescriptorDb(const DescriptorDb&) { }
        ~DescriptorDb() { }
    };
}

#endif