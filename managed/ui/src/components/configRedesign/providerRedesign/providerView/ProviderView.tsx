/*
 * Copyright 2022 YugaByte, Inc. and Contributors
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License")
 * You may not use this file except in compliance with the License. You may obtain a copy of the License at
 * http://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */
import React, { useState } from 'react';
import clsx from 'clsx';
import { ArrowBack } from '@material-ui/icons';
import { Typography } from '@material-ui/core';
import { browserHistory } from 'react-router';
import { useQuery } from 'react-query';
import { DropdownButton, MenuItem } from 'react-bootstrap';

import { DeleteProviderConfigModal } from '../DeleteProviderConfigModal';
import { PROVIDER_ROUTE_PREFIX } from '../constants';
import { ProviderDetails } from './providerDetails/ProviderDetails';
import { YBErrorIndicator, YBLoading } from '../../../common/indicators';
import { YBLabelWithIcon } from '../../../common/descriptors';
import { api, providerQueryKey, universeQueryKey } from '../../../../redesign/helpers/api';
import { getInfraProviderTab, getLinkedUniverses } from '../utils';

import styles from './ProviderView.module.scss';

interface ProviderViewProps {
  providerUUID: string;
}

export const ProviderView = ({ providerUUID }: ProviderViewProps) => {
  const [isDeleteProviderModalOpen, setIsDeleteProviderModalOpen] = useState<boolean>(false);

  const providerQuery = useQuery(providerQueryKey.detail(providerUUID), () =>
    api.fetchProvider(providerUUID)
  );
  const universeListQuery = useQuery(universeQueryKey.ALL, () => api.fetchUniverseList());

  if (
    providerQuery.isLoading ||
    providerQuery.isIdle ||
    universeListQuery.isLoading ||
    universeListQuery.isIdle
  ) {
    return <YBLoading />;
  }

  if (providerQuery.isError) {
    return <YBErrorIndicator customErrorMessage="Error fetching provider." />;
  }
  if (universeListQuery.isError) {
    return <YBErrorIndicator customErrorMessage="Error fetching universe list." />;
  }

  const showDeleteProviderModal = () => {
    setIsDeleteProviderModalOpen(true);
  };
  const hideDeleteProviderModal = () => {
    setIsDeleteProviderModalOpen(false);
  };

  const navigateBack = () => {
    browserHistory.goBack();
  };

  const providerConfig = providerQuery.data;
  const universeList = universeListQuery.data;
  const linkedUniverses = getLinkedUniverses(providerConfig.uuid, universeList);
  return (
    <div className={styles.viewContainer}>
      <div className={styles.header}>
        <ArrowBack className={styles.arrowBack} fontSize="large" onClick={navigateBack} />
        <Typography variant="h4">{providerConfig.name}</Typography>
        <DropdownButton
          bsClass={clsx(styles.actionButton, 'dropdown')}
          title="Actions"
          id="provider-overview-actions"
          pullRight
        >
          <MenuItem
            eventKey="1"
            onSelect={showDeleteProviderModal}
            disabled={linkedUniverses.length > 0}
          >
            <YBLabelWithIcon icon="fa fa-trash">Delete Configuration</YBLabelWithIcon>
          </MenuItem>
        </DropdownButton>
      </div>
      <ProviderDetails linkedUniverses={linkedUniverses} providerConfig={providerConfig} />
      <DeleteProviderConfigModal
        open={isDeleteProviderModalOpen}
        onClose={hideDeleteProviderModal}
        providerConfig={providerConfig}
        redirectURL={`/${PROVIDER_ROUTE_PREFIX}/${getInfraProviderTab(providerConfig)}`}
      />
    </div>
  );
};
